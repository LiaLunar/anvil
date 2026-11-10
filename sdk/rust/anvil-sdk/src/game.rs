//! `anvil-game.game`: the game's main loop, sessions and date, and the events
//! anvil-game publishes on the game's main thread (`anvil_game.h`).
//!
//! Depend on anvil-game in plugin.toml (`[[dependencies]] id = "anvil-game"`)
//! and look the service up with [`Game::get`].

use std::ffi::c_void;
use std::fmt;

use crate::error::{check, Result};
use crate::event::{read_sized, Event, Payload};
use crate::host::Host;
use crate::sys::{
    AnvilGameDate, AnvilGameDateChange, AnvilGameFrame, AnvilGameOosChange, AnvilGamePlayerChange,
    AnvilGameService, AnvilGameSession, AnvilGameWarChange, ANVIL_GAME_SERVICE_VERSION,
    ANVIL_GAME_SESSION_ENDED, ANVIL_GAME_SESSION_STARTED, ANVIL_GAME_WAR_ENDED,
    ANVIL_GAME_WAR_STARTED,
};
use crate::text::{owned_string, Tag};

/// The service's name.
pub const SERVICE: &std::ffi::CStr = c"anvil-game.game";
/// The service's major version.
pub const VERSION: u32 = ANVIL_GAME_SERVICE_VERSION;

/// How a session changed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SessionChange {
    /// A save or a new game is loaded and running.
    Started,
    /// The player left it (to the menu, or by loading another save).
    Ended,
    /// A value this crate does not know.
    Other(u32),
}

/// A game session: `anvil-game.session`'s payload and [`Game::session`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Session {
    /// Started or ended; an end carries the flags the session had.
    pub change: SessionChange,
    /// An ironman game.
    pub ironman: bool,
    /// A multiplayer game.
    pub multiplayer: bool,
    /// 1 for the first session since the game started, then 2, …
    pub number: u64,
}

impl Session {
    fn from_raw(raw: &AnvilGameSession) -> Self {
        let change = match raw.change {
            c if c == ANVIL_GAME_SESSION_STARTED as u32 => SessionChange::Started,
            c if c == ANVIL_GAME_SESSION_ENDED as u32 => SessionChange::Ended,
            other => SessionChange::Other(other),
        };
        Self {
            change,
            ironman: raw.ironman != 0,
            multiplayer: raw.multiplayer != 0,
            number: raw.number,
        }
    }
}

/// An in-game date, as the game's top bar shows it (years have 365 days).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Date {
    /// E.g. 1444.
    pub year: i32,
    /// 1..12.
    pub month: u32,
    /// 1..31.
    pub day: u32,
    /// The [`Session::number`] it belongs to.
    pub session: u64,
}

impl Date {
    fn from_raw(raw: &AnvilGameDate) -> Self {
        Self {
            year: raw.year,
            month: raw.month,
            day: raw.day,
            session: raw.session,
        }
    }
}

impl fmt::Display for Date {
    /// `1444.11.11`, as the game writes dates.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.year, self.month, self.day)
    }
}

/// `anvil-game.frame`'s payload.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Frame {
    /// The game's own frame counter after this frame.
    pub number: u64,
}

/// Out-of-sync flags (`ANVIL_GAME_OOS_*`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct OosFlags(pub u32);

impl OosFlags {
    /// This game's checksums differed from the host's (a client only).
    pub const DETECTED: u32 = crate::sys::ANVIL_GAME_OOS_DETECTED;
    /// The game is paused because a machine went out of sync.
    pub const PAUSED: u32 = crate::sys::ANVIL_GAME_OOS_PAUSED;

    /// Whether the game is out of sync.
    pub const fn detected(self) -> bool {
        self.0 & Self::DETECTED != 0
    }
    /// Whether the game is paused for it.
    pub const fn paused(self) -> bool {
        self.0 & Self::PAUSED != 0
    }
}

/// What of the game's GUI takes the keyboard (`ANVIL_GAME_KEYBOARD_*`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct KeyboardFlags(pub u32);

impl KeyboardFlags {
    /// A text field of the game has focus.
    pub const TEXT: u32 = crate::sys::ANVIL_GAME_KEYBOARD_TEXT;
    /// The shortcut editor waits for a key.
    pub const SHORTCUT: u32 = crate::sys::ANVIL_GAME_KEYBOARD_SHORTCUT;

    /// Whether a text field has focus.
    pub const fn text(self) -> bool {
        self.0 & Self::TEXT != 0
    }
    /// Whether the shortcut editor waits.
    pub const fn shortcut(self) -> bool {
        self.0 & Self::SHORTCUT != 0
    }
}

/// `anvil-game.oos`'s payload.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OosChange {
    /// The flags now.
    pub flags: OosFlags,
    /// The flags before.
    pub previous: OosFlags,
    /// The [`Session::number`].
    pub session: u64,
}

/// The payload of `anvil-game.day`, `.month` and `.year`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DateChange {
    /// How many days the step covers: 1 in a single-player game, perhaps more
    /// on a multiplayer client.
    pub days: u32,
    /// The date now.
    pub date: Date,
    /// The date before.
    pub previous: Date,
}

/// `anvil-game.player`'s payload: the player's country changed within a session.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlayerChange {
    /// The country now; `None` for none.
    pub tag: Option<Tag>,
    /// The country before.
    pub previous: Option<Tag>,
    /// The [`Session::number`].
    pub session: u64,
}

/// How a war changed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum WarChangeKind {
    /// The war started.
    Started,
    /// The war ended.
    Ended,
    /// A value this crate does not know.
    Other(u32),
}

/// `anvil-game.war`'s payload.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WarChange {
    /// Started or ended.
    pub change: WarChangeKind,
    /// The war's id (no longer valid for an end).
    pub war: u64,
    /// Who started it.
    pub attacker: Option<Tag>,
    /// Whom it was declared on.
    pub defender: Option<Tag>,
    /// The war's name as the game shows it.
    pub name: String,
    /// The [`Session::number`].
    pub session: u64,
}

// SAFETY: anvil-game.frame publishes a const AnvilGameFrame*.
unsafe impl Payload for Frame {
    unsafe fn read(raw: *const c_void) -> Option<Self> {
        // SAFETY: the event's payload type.
        unsafe { read_sized::<AnvilGameFrame>(raw) }.map(|f| Self { number: f.number })
    }
}

// SAFETY: anvil-game.session publishes a const AnvilGameSession*.
unsafe impl Payload for Session {
    unsafe fn read(raw: *const c_void) -> Option<Self> {
        // SAFETY: the event's payload type.
        unsafe { read_sized::<AnvilGameSession>(raw) }.map(|s| Self::from_raw(&s))
    }
}

// SAFETY: anvil-game.oos publishes a const AnvilGameOosChange*.
unsafe impl Payload for OosChange {
    unsafe fn read(raw: *const c_void) -> Option<Self> {
        // SAFETY: the event's payload type.
        unsafe { read_sized::<AnvilGameOosChange>(raw) }.map(|c| Self {
            flags: OosFlags(c.flags),
            previous: OosFlags(c.previous),
            session: c.session,
        })
    }
}

// SAFETY: anvil-game.day, .month and .year publish a const AnvilGameDateChange*.
unsafe impl Payload for DateChange {
    unsafe fn read(raw: *const c_void) -> Option<Self> {
        // SAFETY: the event's payload type.
        unsafe { read_sized::<AnvilGameDateChange>(raw) }.map(|c| Self {
            days: c.days,
            date: Date::from_raw(&c.date),
            previous: Date::from_raw(&c.previous),
        })
    }
}

// SAFETY: anvil-game.player publishes a const AnvilGamePlayerChange*.
unsafe impl Payload for PlayerChange {
    unsafe fn read(raw: *const c_void) -> Option<Self> {
        // SAFETY: the event's payload type.
        unsafe { read_sized::<AnvilGamePlayerChange>(raw) }.map(|c| Self {
            tag: Tag::from_raw(&c.tag),
            previous: Tag::from_raw(&c.previous),
            session: c.session,
        })
    }
}

// SAFETY: anvil-game.war publishes a const AnvilGameWarChange* with its name.
unsafe impl Payload for WarChange {
    unsafe fn read(raw: *const c_void) -> Option<Self> {
        // SAFETY: the event's payload type; a zeroed name (an older publisher) is null.
        let c = unsafe { read_sized::<AnvilGameWarChange>(raw) }?;
        let change = match c.change {
            k if k == ANVIL_GAME_WAR_STARTED as u32 => WarChangeKind::Started,
            k if k == ANVIL_GAME_WAR_ENDED as u32 => WarChangeKind::Ended,
            other => WarChangeKind::Other(other),
        };
        Some(Self {
            change,
            war: c.war,
            attacker: Tag::from_raw(&c.attacker),
            defender: Tag::from_raw(&c.defender),
            // SAFETY: the name is valid during the handler.
            name: unsafe { owned_string(c.name) },
            session: c.session,
        })
    }
}

/// Once, when the game has finished initializing (the main menu is up).
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const READY: Event<()> = unsafe { Event::new(c"anvil-game.ready") };
/// After every frame of the main loop. Keep the handler short.
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const FRAME: Event<Frame> = unsafe { Event::new(c"anvil-game.frame") };
/// A session started or ended.
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const SESSION: Event<Session> = unsafe { Event::new(c"anvil-game.session") };
/// The main menu came up or went away (anvil-game 1.8.0); ask [`Game::main_menu`].
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const MAIN_MENU: Event<()> = unsafe { Event::new(c"anvil-game.main_menu") };
/// The out-of-sync flags changed (anvil-game 1.14.0).
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const OOS: Event<OosChange> = unsafe { Event::new(c"anvil-game.oos") };
/// The date moved (anvil-game 1.11.0).
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const DAY: Event<DateChange> = unsafe { Event::new(c"anvil-game.day") };
/// A new month began (anvil-game 1.11.0), after the game's monthly update.
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const MONTH: Event<DateChange> = unsafe { Event::new(c"anvil-game.month") };
/// A new year began (anvil-game 1.11.0), after the game's yearly update.
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const YEAR: Event<DateChange> = unsafe { Event::new(c"anvil-game.year") };
/// The player's country changed within a session (anvil-game 1.11.0).
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const PLAYER: Event<PlayerChange> = unsafe { Event::new(c"anvil-game.player") };
/// A war started or ended (anvil-game 1.11.0).
// SAFETY: anvil_game.h names the payload type, which this one reads.
pub const WAR: Event<WarChange> = unsafe { Event::new(c"anvil-game.war") };

/// The `anvil-game.game` service.
#[derive(Clone, Copy)]
pub struct Game {
    table: &'static AnvilGameService,
}

impl fmt::Debug for Game {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Game")
            .field("size", &self.table.size)
            .finish()
    }
}

struct Work<F> {
    work: F,
}

unsafe extern "C" fn run_work<F: FnOnce() + Send + 'static>(context: *mut c_void) {
    // SAFETY: the box run_on_main_thread queued, run once.
    let work = unsafe { Box::from_raw(context.cast::<Work<F>>()) };
    let logged = std::sync::atomic::AtomicBool::new(false);
    crate::panic::run_callback("run_on_main_thread work", &logged, (), move || {
        (work.work)()
    });
}

impl Game {
    /// The service; `None` without anvil-game.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-game.game version 1.
        let table = unsafe { host.service::<AnvilGameService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilGameService {
        self.table
    }

    /// Whether `anvil-game.ready` was published.
    pub fn ready(&self) -> bool {
        // SAFETY: the service's function.
        function!(self.table, AnvilGameService, ready).is_ok_and(|f| unsafe { f() } != 0)
    }

    /// The game's main thread; 0 before its first frame.
    pub fn main_thread_id(&self) -> u32 {
        // SAFETY: as above.
        function!(self.table, AnvilGameService, main_thread_id).map_or(0, |f| unsafe { f() })
    }

    /// The frame number of the last `anvil-game.frame`; 0 before it.
    pub fn frame_number(&self) -> u64 {
        // SAFETY: as above.
        function!(self.table, AnvilGameService, frame_number).map_or(0, |f| unsafe { f() })
    }

    /// Queues `work` to run on the main thread after the next frame of the
    /// main loop, in the order queued. Callable from any thread. Work still
    /// queued when the game quits never runs, and frames stop while a save
    /// loads: never wait for it without a limit.
    pub fn run_on_main_thread(&self, work: impl FnOnce() + Send + 'static) -> Result<()> {
        self.queue(work)
    }

    fn queue<F: FnOnce() + Send + 'static>(&self, work: F) -> Result<()> {
        let call = function!(self.table, AnvilGameService, run_on_main_thread)?;
        let context = Box::into_raw(Box::new(Work { work }));
        // SAFETY: the adapter never unwinds and frees the box when it runs.
        let result = unsafe { call(Some(run_work::<F>), context.cast()) };
        check(result, || "run_on_main_thread").inspect_err(|_| {
            // SAFETY: refused, so it never runs: the box is still ours.
            drop(unsafe { Box::from_raw(context) });
        })
    }

    /// The running session, as of the last frame of the main loop.
    pub fn session(&self) -> Option<Session> {
        let f = function!(self.table, AnvilGameService, session).ok()?;
        // SAFETY: zeroed plain data with its size set, filled by the service.
        let mut raw: AnvilGameSession = unsafe { std::mem::zeroed() };
        raw.size = std::mem::size_of::<AnvilGameSession>() as u32;
        // SAFETY: the service's function with a valid out pointer.
        (unsafe { f(&mut raw) } != 0).then(|| Session::from_raw(&raw))
    }

    /// The current date while a session runs (anvil-game 1.6.0); `None` in
    /// the menu, while a save loads from it, or with an older anvil-game.
    pub fn date(&self) -> Option<Date> {
        let f = function!(self.table, AnvilGameService, date).ok()?;
        // SAFETY: as in session().
        let mut raw: AnvilGameDate = unsafe { std::mem::zeroed() };
        raw.size = std::mem::size_of::<AnvilGameDate>() as u32;
        // SAFETY: as in session().
        (unsafe { f(&mut raw) } != 0).then(|| Date::from_raw(&raw))
    }

    /// Whether the main menu is up (anvil-game 1.8.0); `None` with an older one.
    pub fn main_menu(&self) -> Option<bool> {
        // SAFETY: the service's function.
        function!(self.table, AnvilGameService, main_menu)
            .ok()
            .map(|f| unsafe { f() } != 0)
    }

    /// The session's out-of-sync flags (anvil-game 1.14.0); `None` with an older one.
    pub fn out_of_sync(&self) -> Option<OosFlags> {
        // SAFETY: the service's function.
        function!(self.table, AnvilGameService, out_of_sync)
            .ok()
            .map(|f| OosFlags(unsafe { f() }))
    }

    /// Whether the game's GUI takes the keyboard for itself (anvil-game
    /// 1.18.0), read live: ask on the main thread. `None` with an older
    /// anvil-game or on a build that declares its symbols absent.
    pub fn keyboard(&self) -> Option<KeyboardFlags> {
        let f = function!(self.table, AnvilGameService, keyboard).ok()?;
        let mut flags = 0u32;
        // SAFETY: the service's function with a valid out pointer.
        (unsafe { f(&mut flags) } != 0).then_some(KeyboardFlags(flags))
    }
}
