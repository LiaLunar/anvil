//! `anvil-game.commands`: the commands a player's click on the unit panel
//! would post, through the game's own queue (anvil-game 1.17.0).
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
    ANVIL_GAME_COMMAND_MAX_PER_FRAME, ANVIL_GAME_COMMAND_MAX_UNITS, ANVIL_GAME_COMMAND_MERGE,
    ANVIL_GAME_COMMAND_SPLIT, ANVIL_GAME_COMMAND_START, ANVIL_GAME_UNIT_ARMY, ANVIL_GAME_UNIT_NAVY,
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
    /// The service, posting for this plugin; `None` without anvil-game 1.17.0
    /// or on a game build whose symbols for it are declared absent.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-game.commands version 1.
        let table = unsafe { host.service::<AnvilGameCommandsService>(SERVICE, VERSION) }?;
        Some(Self {
            table,
            plugin: host.raw_plugin(),
        })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilGameCommandsService {
        self.table
    }

    fn post(&self, kind: u32, flags: u32, units: &[UnitRef], what: &str) -> Result<Posted> {
        let f = function!(self.table, AnvilGameCommandsService, post)?;
        let raw: Vec<AnvilGameUnitRef> = units.iter().map(raw_ref).collect();
        let command = AnvilGameCommand {
            size: size_of::<AnvilGameCommand>() as u32,
            type_: kind,
            flags,
            // Over u32::MAX is over the limit too: the service answers Limit.
            count: u32::try_from(raw.len()).unwrap_or(u32::MAX),
            units: raw.as_ptr(),
        };
        let mut out = AnvilGameCommandResult {
            size: size_of::<AnvilGameCommandResult>() as u32,
            posted: 0,
            skipped: 0,
            refused: 0,
        };
        // SAFETY: this plugin's handle, a command whose units live until the
        // call returns, and a result the service fills as far as its size says.
        check(unsafe { f(self.plugin, &command, &mut out) }, || {
            format!("commands {what} ({} units)", units.len())
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
}
