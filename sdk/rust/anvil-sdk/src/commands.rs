//! `anvil-game.commands`: the commands a player's click on the unit panel
//! would post, through the game's own queue, and the choice of an event
//! option as its button makes it (anvil-game 1.17.0, service versions 1 and 2),
//! and the game speed as the speed bar's buttons set it (anvil-game 1.22.0,
//! version 3).
//!
//! **Who.** Only a plugin with `affects_gameplay = true`, and only while Anvil
//! has not suspended it (ironman or multiplayer without the player's opt-in):
//! otherwise [`ErrorCode::Unsupported`] or [`ErrorCode::Suspended`], and
//! nothing reaches the game.
//!
//! **What.** Only the player's own units, as [`UnitRef`]s from
//! [`World`](crate::world::World). A unit that is gone or another country's is
//! [`ErrorCode::NotFound`]; one of the wrong kind, given twice, or a bad
//! combination is [`ErrorCode::InvalidArgument`]. Nothing of such a call is
//! posted.
//!
//! **When.** On the game's main thread after an outer frame, in a session, as
//! for `World`; anywhere else, and during a hot-join,
//! [`ErrorCode::WrongPhase`]. At most [`MAX_UNITS`] units a call and
//! [`MAX_PER_FRAME`] commands a frame for all plugins together, above that
//! [`ErrorCode::Limit`].
//!
//! **Posted is not done.** A posted command runs at the session's next turn
//! of the queue on every machine; in multiplayer it is only on its way to the
//! host. Read `World` a few frames later for the outcome.
//!
//! [`ErrorCode::Unsupported`]: crate::ErrorCode::Unsupported
//! [`ErrorCode::Suspended`]: crate::ErrorCode::Suspended
//! [`ErrorCode::NotFound`]: crate::ErrorCode::NotFound
//! [`ErrorCode::InvalidArgument`]: crate::ErrorCode::InvalidArgument
//! [`ErrorCode::WrongPhase`]: crate::ErrorCode::WrongPhase
//! [`ErrorCode::Limit`]: crate::ErrorCode::Limit

use std::fmt;
use std::mem::size_of;

use crate::error::{check, Result};
use crate::host::Host;
use crate::sys::{
    AnvilGameCommand, AnvilGameCommandResult, AnvilGameCommandsService, AnvilGameUnitRef,
    AnvilPlugin, ANVIL_GAME_COMMANDS_SERVICE_VERSION, ANVIL_GAME_COMMAND_DRILL,
    ANVIL_GAME_COMMAND_EVENT_OPTION, ANVIL_GAME_COMMAND_EXPLORE, ANVIL_GAME_COMMAND_MAX_PER_FRAME,
    ANVIL_GAME_COMMAND_MAX_UNITS, ANVIL_GAME_COMMAND_MERGE, ANVIL_GAME_COMMAND_MOVE,
    ANVIL_GAME_COMMAND_QUEUE, ANVIL_GAME_COMMAND_SIEGE_AREA, ANVIL_GAME_COMMAND_SPEED,
    ANVIL_GAME_COMMAND_SPLIT, ANVIL_GAME_COMMAND_START, ANVIL_GAME_COMMAND_SUPPRESS_AREA,
    ANVIL_GAME_EXPLORE_CIRCUMNAVIGATE, ANVIL_GAME_EXPLORE_LAND, ANVIL_GAME_EXPLORE_STOP,
    ANVIL_GAME_UNIT_ARMY, ANVIL_GAME_UNIT_NAVY,
};
use crate::world::{UnitKind, UnitRef};

/// The service's name.
pub const SERVICE: &std::ffi::CStr = c"anvil-game.commands";
/// The service's major version.
pub const VERSION: u32 = ANVIL_GAME_COMMANDS_SERVICE_VERSION;
/// The most units one call takes.
pub const MAX_UNITS: usize = ANVIL_GAME_COMMAND_MAX_UNITS as usize;
/// The most commands one frame takes, for all plugins together.
pub const MAX_PER_FRAME: u32 = ANVIL_GAME_COMMAND_MAX_PER_FRAME;

/// What one call posted.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Posted {
    /// Commands the session took (in multiplayer: sent to the host).
    pub posted: u32,
    /// Units already in the asked state: nothing posted for them.
    pub skipped: u32,
    /// Commands the session's own check refused; the service freed them.
    pub refused: u32,
}

/// The `anvil-game.commands` service, for this plugin.
#[derive(Clone, Copy)]
pub struct Commands {
    table: &'static AnvilGameCommandsService,
    plugin: *mut AnvilPlugin,
    version: u32,
}

// SAFETY: the table lives for the process and its function may be called from
// any thread (it answers WrongPhase off the main thread); the plugin handle is
// Anvil's, valid for the process, and only passed back to it.
unsafe impl Send for Commands {}
// SAFETY: as for Send; the value is never written.
unsafe impl Sync for Commands {}

impl fmt::Debug for Commands {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Commands")
            .field("size", &self.table.size)
            .finish()
    }
}

fn raw_ref(unit: &UnitRef) -> AnvilGameUnitRef {
    AnvilGameUnitRef {
        id: unit.id,
        country: unit
            .country
            .map(|tag| tag.to_raw())
            .unwrap_or(crate::sys::AnvilGameTag { text: [0; 4] }),
        kind: match unit.kind {
            UnitKind::Army => ANVIL_GAME_UNIT_ARMY,
            UnitKind::Navy => ANVIL_GAME_UNIT_NAVY,
            UnitKind::Other(other) => other,
        },
    }
}

impl Commands {
    /// The service, posting for this plugin: the newest version anvil-game
    /// has (3, 2, then 1); `None` without anvil-game 1.17.0 or on a game build
    /// whose symbols for it are declared absent.
    pub fn get(host: &Host) -> Option<Self> {
        [VERSION, 2, 1].into_iter().find_map(|version| {
            // SAFETY: the table type of anvil-game.commands; each version only appends.
            let table = unsafe { host.service::<AnvilGameCommandsService>(SERVICE, version) }?;
            Some(Self {
                table,
                plugin: host.raw_plugin(),
                version,
            })
        })
    }

    /// The service version found: 2 chooses event options, 3 also sets the
    /// game speed.
    pub fn version(&self) -> u32 {
        self.version
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilGameCommandsService {
        self.table
    }

    fn post(&self, kind: u32, flags: u32, units: &[UnitRef], what: &str) -> Result<Posted> {
        self.post_with(kind, flags, units, 0, 0, what)
    }

    fn post_with(
        &self,
        kind: u32,
        flags: u32,
        units: &[UnitRef],
        explore: i32,
        province: i32,
        what: &str,
    ) -> Result<Posted> {
        let raw: Vec<AnvilGameUnitRef> = units.iter().map(raw_ref).collect();
        let command = AnvilGameCommand {
            // Version 1 takes the fields up to `units` only.
            size: if self.version >= 2 {
                size_of::<AnvilGameCommand>()
            } else {
                std::mem::offset_of!(AnvilGameCommand, pending_event_id)
            } as u32,
            type_: kind,
            flags,
            // Over u32::MAX is over the limit too: the service answers Limit.
            count: u32::try_from(raw.len()).unwrap_or(u32::MAX),
            units: if raw.is_empty() {
                std::ptr::null()
            } else {
                raw.as_ptr()
            },
            pending_event_id: 0,
            option_index: 0,
            speed: 0,
            explore,
            province,
        };
        self.send(&command, what, units.len())
    }

    fn send(&self, command: &AnvilGameCommand, what: &str, units: usize) -> Result<Posted> {
        let f = function!(self.table, AnvilGameCommandsService, post)?;
        let mut out = AnvilGameCommandResult {
            size: size_of::<AnvilGameCommandResult>() as u32,
            posted: 0,
            skipped: 0,
            refused: 0,
        };
        // SAFETY: this plugin's handle, a command whose units live until the
        // call returns, and a result the service fills as far as its size says.
        check(unsafe { f(self.plugin, command, &mut out) }, || {
            format!("commands {what} ({units} units)")
        })?;
        Ok(Posted {
            posted: out.posted,
            skipped: out.skipped,
            refused: out.refused,
        })
    }

    /// Armies: start drilling (`start`) or stop. Only the armies not yet in
    /// that state get a command; the others count as `skipped`.
    pub fn drill(&self, armies: &[UnitRef], start: bool) -> Result<Posted> {
        let flags = if start { ANVIL_GAME_COMMAND_START } else { 0 };
        self.post(ANVIL_GAME_COMMAND_DRILL, flags, armies, "drill")
    }

    /// Two or more armies, or navies, in one province: merged into the first.
    pub fn merge(&self, units: &[UnitRef]) -> Result<Posted> {
        self.post(ANVIL_GAME_COMMAND_MERGE, 0, units, "merge")
    }

    /// One army or navy: split in half, rounded up, as the button does.
    pub fn split(&self, unit: &UnitRef) -> Result<Posted> {
        self.post(
            ANVIL_GAME_COMMAND_SPLIT,
            0,
            std::slice::from_ref(unit),
            "split",
        )
    }

    /// Version 2: chooses `option` (the event file's order, from 0) of the
    /// local player's pending event `pending_event_id`, as its button does.
    /// `NotFound` without its event window, `InvalidArgument` for an option
    /// without a button, `WrongPhase` while the button is disabled (the half
    /// second after the window opens: try a frame later); `skipped` when the
    /// window is already chosen, `refused` when the session would not take it.
    /// `Unsupported` on version 1.
    pub fn choose_event_option(&self, pending_event_id: i32, option: i32) -> Result<Posted> {
        if self.version < 2 {
            return Err(crate::Error::new(
                crate::ErrorCode::Unsupported,
                "commands event option: anvil-game.commands version 1",
            ));
        }
        let command = AnvilGameCommand {
            size: size_of::<AnvilGameCommand>() as u32,
            type_: ANVIL_GAME_COMMAND_EVENT_OPTION,
            flags: 0,
            count: 0,
            units: std::ptr::null(),
            pending_event_id,
            option_index: option,
            speed: 0,
            explore: 0,
            province: 0,
        };
        self.send(&command, "event option", 0)
    }

    /// Version 3: sets the game speed to `speed` (1 the slowest .. 5), as that
    /// many clicks on the speed bar's + or - would: one command a step.
    /// `skipped` 1 at that speed already; `refused` 1 when the bar's buttons
    /// would do nothing (the in-game menu is shown, or a multiplayer client
    /// without admin rights); `InvalidArgument` outside 1..5. `Unsupported`
    /// before version 3.
    pub fn set_speed(&self, speed: i32) -> Result<Posted> {
        if self.version < 3 {
            return Err(crate::Error::new(
                crate::ErrorCode::Unsupported,
                "commands speed: anvil-game.commands before version 3",
            ));
        }
        let command = AnvilGameCommand {
            size: size_of::<AnvilGameCommand>() as u32,
            type_: ANVIL_GAME_COMMAND_SPEED,
            flags: 0,
            count: 0,
            units: std::ptr::null(),
            pending_event_id: 0,
            option_index: 0,
            speed,
            explore: 0,
            province: 0,
        };
        self.send(&command, "speed", 0)
    }

    /// Version 3: armies (`Explore::Land`) or fleets (`Explore::Circumnavigate`)
    /// start exploring, as the unit panel's button and the exploration dialog
    /// do; `Explore::Stop` stops the units that explore. Units already in the
    /// asked state count as `skipped`. The game checks each command when it
    /// runs it. `Unsupported` before version 3.
    pub fn explore(&self, units: &[UnitRef], how: Explore) -> Result<Posted> {
        self.need_version_3("explore")?;
        let mission = match how {
            Explore::Stop => ANVIL_GAME_EXPLORE_STOP,
            Explore::Land => ANVIL_GAME_EXPLORE_LAND,
            Explore::Circumnavigate => ANVIL_GAME_EXPLORE_CIRCUMNAVIGATE,
        };
        self.post_with(
            ANVIL_GAME_COMMAND_EXPLORE,
            0,
            units,
            mission as i32,
            0,
            "explore",
        )
    }

    /// Version 3: armies or fleets move to `province`, as a right-click on the
    /// map does for a plain move: the path is replaced, or added to with
    /// `queue` (as Shift). A move the game does not allow now counts as
    /// `refused`. `Unsupported` before version 3.
    pub fn move_to(&self, units: &[UnitRef], province: i32, queue: bool) -> Result<Posted> {
        self.need_version_3("move")?;
        let flags = if queue { ANVIL_GAME_COMMAND_QUEUE } else { 0 };
        self.post_with(ANVIL_GAME_COMMAND_MOVE, flags, units, 0, province, "move")
    }

    /// Version 3: each army's click on the map area of `province` in the siege
    /// map mode: the area joins the army's autonomous siege, or leaves it when
    /// it is in it already. The game checks the area for each army and posts
    /// the command itself; an army it says no for counts as `refused`.
    /// `NotFound` for a province without a map area.
    pub fn siege_area(&self, armies: &[UnitRef], province: i32) -> Result<Posted> {
        self.need_version_3("siege area")?;
        self.post_with(
            ANVIL_GAME_COMMAND_SIEGE_AREA,
            0,
            armies,
            0,
            province,
            "siege area",
        )
    }

    /// Version 3: the same in the rebel-suppression map mode: the area joins
    /// the army's rebel hunt, or leaves it.
    pub fn suppress_area(&self, armies: &[UnitRef], province: i32) -> Result<Posted> {
        self.need_version_3("suppress area")?;
        self.post_with(
            ANVIL_GAME_COMMAND_SUPPRESS_AREA,
            0,
            armies,
            0,
            province,
            "suppress area",
        )
    }

    fn need_version_3(&self, what: &str) -> Result<()> {
        if self.version < 3 {
            return Err(crate::Error::new(
                crate::ErrorCode::Unsupported,
                format!("commands {what}: anvil-game.commands before version 3"),
            ));
        }
        Ok(())
    }
}

/// What [`Commands::explore`] asks of the units.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Explore {
    /// Units that explore (land, sea or a circumnavigation) stop.
    Stop,
    /// Armies explore the land around them.
    Land,
    /// Fleets sail around the world.
    Circumnavigate,
}
