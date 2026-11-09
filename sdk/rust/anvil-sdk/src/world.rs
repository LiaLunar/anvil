//! `anvil-game.world`: the world of the running session, read straight from
//! the game (anvil-game 1.11.0; modifiers, units and the selection 1.15.0).
//!
//! **Thread.** Call it only on the game's main thread after an outer frame:
//! from a handler of an anvil-game event ([`game`](crate::game)) or from
//! [`Game::run_on_main_thread`](crate::game::Game::run_on_main_thread) work,
//! or from a panel click. Anywhere else, and outside a session, a call answers
//! [`ErrorCode::WrongPhase`] and reads nothing.
//!
//! **Results.** [`ErrorCode::NotFound`] for an unknown province ID, tag, war
//! or unit; [`ErrorCode::NotDeclared`] for a group of calls whose symbols a
//! port declared absent; [`ErrorCode::Unsupported`] for a call the running
//! anvil-game is too old to have.
//!
//! **What it shows.** Everything, also under terra incognita and in
//! multiplayer, where that is more than the player sees. A read changes
//! nothing, so it cannot cause a desync. Values marked "× 1000" are as the
//! game stores them, as [`Milli`].
//!
//! [`ErrorCode::WrongPhase`]: crate::ErrorCode::WrongPhase
//! [`ErrorCode::NotFound`]: crate::ErrorCode::NotFound
//! [`ErrorCode::NotDeclared`]: crate::ErrorCode::NotDeclared
//! [`ErrorCode::Unsupported`]: crate::ErrorCode::Unsupported

use std::fmt;
use std::mem::size_of;

use crate::error::{check, Result};
use crate::host::Host;
use crate::sys::{
    AnvilGameCountry, AnvilGameProvince, AnvilGameTag, AnvilGameUnit, AnvilGameUnitRef,
    AnvilGameWar, AnvilGameWorldService, ANVIL_GAME_COUNTRY_OBSERVER, ANVIL_GAME_PROVINCE_LAND,
    ANVIL_GAME_PROVINCE_WASTELAND, ANVIL_GAME_UNIT_ARMY, ANVIL_GAME_UNIT_NAVY,
    ANVIL_GAME_VALUE_MANPOWER, ANVIL_GAME_VALUE_MONTHLY_INCOME, ANVIL_GAME_VALUE_PRESTIGE,
    ANVIL_GAME_VALUE_STABILITY, ANVIL_GAME_VALUE_TREASURY, ANVIL_GAME_WAR_ATTACKERS,
    ANVIL_GAME_WAR_DEFENDERS, ANVIL_GAME_WORLD_SERVICE_VERSION,
};
use crate::text::{read_list, read_text, Tag};

/// The service's name.
pub const SERVICE: &std::ffi::CStr = c"anvil-game.world";
/// The service's major version.
pub const VERSION: u32 = ANVIL_GAME_WORLD_SERVICE_VERSION;

/// A value the game stores × 1000: `Milli(4500)` is 4.5.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Milli(pub i32);

impl Milli {
    /// The value as a float.
    pub fn get(self) -> f64 {
        f64::from(self.0) / 1000.0
    }
}

impl fmt::Display for Milli {
    /// With up to three decimals, trailing zeros dropped: `4.5`, `-12`, `0.125`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sign = if self.0 < 0 { "-" } else { "" };
        let value = self.0.unsigned_abs();
        let (whole, part) = (value / 1000, value % 1000);
        if part == 0 {
            return write!(f, "{sign}{whole}");
        }
        let decimals = format!("{part:03}");
        write!(f, "{sign}{whole}.{}", decimals.trim_end_matches('0'))
    }
}

/// A province.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Province {
    /// Its ID.
    pub id: i32,
    /// Its owner; `None` for none.
    pub owner: Option<Tag>,
    /// Its controller.
    pub controller: Option<Tag>,
    /// Base tax, without the capital's bonus.
    pub base_tax: Milli,
    /// Base production.
    pub base_production: Milli,
    /// Base manpower.
    pub base_manpower: Milli,
    /// Not a sea or a lake.
    pub land: bool,
    /// A wasteland.
    pub wasteland: bool,
}

/// A country. A value this game build does not have is `None`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Country {
    /// Its tag.
    pub tag: Option<Tag>,
    /// Its capital province; `None` for none.
    pub capital: Option<i32>,
    /// The provinces it owns; above 0 when it exists on the map.
    pub province_count: i32,
    /// The observer's pseudo-country.
    pub observer: bool,
    /// Ducats.
    pub treasury: Option<Milli>,
    /// Stability.
    pub stability: Option<Milli>,
    /// Prestige.
    pub prestige: Option<Milli>,
    /// Manpower in the game's thousands: `Milli(25000)` is 25 000 men.
    pub manpower: Option<Milli>,
    /// The income total of the economy view, before expenses, as of the last
    /// monthly update.
    pub monthly_income: Option<Milli>,
}

/// A running war.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct War {
    /// Its id: the same while the war runs (a later war may reuse it).
    pub id: u64,
    /// Who started it.
    pub attacker: Option<Tag>,
    /// Whom it was declared on.
    pub defender: Option<Tag>,
}

/// A side of a war.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Side {
    /// The attacker and its allies.
    Attackers,
    /// The defender and its allies.
    Defenders,
}

/// An army or a navy.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum UnitKind {
    /// An army.
    Army,
    /// A navy.
    Navy,
    /// A value this crate does not know.
    Other(u32),
}

impl UnitKind {
    fn from_raw(kind: u32) -> Self {
        match kind {
            ANVIL_GAME_UNIT_ARMY => Self::Army,
            ANVIL_GAME_UNIT_NAVY => Self::Navy,
            other => Self::Other(other),
        }
    }
}

/// What a unit does on its own, by the game's numbers (`ANVIL_GAME_MISSION_*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Mission(pub i32);

impl Mission {
    /// Not one the service could read.
    pub const UNKNOWN: Self = Self(crate::sys::ANVIL_GAME_MISSION_UNKNOWN);
    /// No mission: the player moves it.
    pub const NONE: Self = Self(crate::sys::ANVIL_GAME_MISSION_NONE as i32);
    /// Besiege the enemy's provinces on its own.
    pub const SIEGE: Self = Self(crate::sys::ANVIL_GAME_MISSION_SIEGE as i32);
    /// Hunt rebels.
    pub const HUNT_REBELS: Self = Self(crate::sys::ANVIL_GAME_MISSION_HUNT_REBELS as i32);
    /// Drill.
    pub const DRILL: Self = Self(crate::sys::ANVIL_GAME_MISSION_DRILL as i32);
}

/// A unit: an army or a navy.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Unit {
    /// Army or navy.
    pub kind: UnitKind,
    /// The game's id: the same while the unit exists.
    pub id: u64,
    /// The country it belongs to.
    pub country: Option<Tag>,
    /// The province it is in or at; `None` for none.
    pub location: Option<i32>,
    /// What it does on its own.
    pub mission: Mission,
    /// Regiments or ships.
    pub subunits: u32,
    /// The sum of the regiments' or ships' strength (men for an army).
    pub strength: Milli,
    /// Its leader's id; `None` for no leader.
    pub leader: Option<u64>,
}

/// One unit of the player's selection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UnitRef {
    /// The unit's id.
    pub id: u64,
    /// The country it belongs to (a player may select units of others to look).
    pub country: Option<Tag>,
    /// Army or navy.
    pub kind: UnitKind,
}

/// The id of a modifier, from its script key ([`World::modifier_id`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ModifierId(pub i32);

/// The `anvil-game.world` service.
#[derive(Clone, Copy)]
pub struct World {
    table: &'static AnvilGameWorldService,
}

impl fmt::Debug for World {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("World")
            .field("size", &self.table.size)
            .finish()
    }
}

fn sized<T>() -> T {
    // SAFETY: the world's structs are plain data for which zero is valid.
    let mut value: T = unsafe { std::mem::zeroed() };
    let size = size_of::<T>() as u32;
    // SAFETY: each of them starts with `uint32_t size`.
    unsafe { std::ptr::from_mut(&mut value).cast::<u32>().write(size) };
    value
}

fn known(value: i32, values: u32, bit: u32) -> Option<Milli> {
    (values & bit != 0).then_some(Milli(value))
}

const EMPTY_TAG: AnvilGameTag = AnvilGameTag { text: [0; 4] };

impl World {
    /// The service; `None` without anvil-game 1.11.0 or on a game build whose
    /// symbols for it are declared absent.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-game.world version 1.
        let table = unsafe { host.service::<AnvilGameWorldService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilGameWorldService {
        self.table
    }

    /// The country the game treats as the player's (the observed one in observe mode).
    pub fn player(&self) -> Result<Option<Tag>> {
        let f = function!(self.table, AnvilGameWorldService, player)?;
        let mut tag = EMPTY_TAG;
        // SAFETY: the service's function with an out pointer.
        check(unsafe { f(&mut tag) }, || "world player")?;
        Ok(Tag::from_raw(&tag))
    }

    /// Valid province IDs are `1..limit`; 0 where a call is not allowed.
    pub fn province_limit(&self) -> i32 {
        // SAFETY: the service's function.
        function!(self.table, AnvilGameWorldService, province_limit).map_or(0, |f| unsafe { f() })
    }

    /// A province.
    pub fn province(&self, id: i32) -> Result<Province> {
        let f = function!(self.table, AnvilGameWorldService, province)?;
        let mut raw: AnvilGameProvince = sized();
        // SAFETY: the service's function; `size` says how much it may write.
        check(unsafe { f(id, &mut raw) }, || {
            format!("world province {id}")
        })?;
        Ok(Province {
            id: raw.id,
            owner: Tag::from_raw(&raw.owner),
            controller: Tag::from_raw(&raw.controller),
            base_tax: Milli(raw.base_tax),
            base_production: Milli(raw.base_production),
            base_manpower: Milli(raw.base_manpower),
            land: raw.flags & ANVIL_GAME_PROVINCE_LAND != 0,
            wasteland: raw.flags & ANVIL_GAME_PROVINCE_WASTELAND != 0,
        })
    }

    /// The name the game displays (it follows the owner's culture).
    pub fn province_name(&self, id: i32) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, province_name)?;
        // SAFETY: the service fills at most `capacity` bytes.
        read_text(
            || format!("world province_name {id}"),
            |buffer, capacity, length| unsafe { f(id, buffer, capacity, length) },
        )
    }

    /// The tags with a core on the province.
    pub fn province_cores(&self, id: i32) -> Result<Vec<Tag>> {
        let f = function!(self.table, AnvilGameWorldService, province_cores)?;
        // SAFETY: the service fills at most `capacity` tags.
        let raw = read_list(
            EMPTY_TAG,
            || format!("world province_cores {id}"),
            |out, capacity, count| unsafe { f(id, out, capacity, count) },
        )?;
        Ok(raw.iter().filter_map(Tag::from_raw).collect())
    }

    /// A country (the tag ignores case).
    pub fn country(&self, tag: Tag) -> Result<Country> {
        let f = function!(self.table, AnvilGameWorldService, country)?;
        let text = tag.c_text();
        let mut raw: AnvilGameCountry = sized();
        // SAFETY: a NUL-terminated tag; `size` says how much it may write.
        check(unsafe { f(text.as_ptr(), &mut raw) }, || {
            format!("world country {tag}")
        })?;
        let v = raw.values;
        Ok(Country {
            tag: Tag::from_raw(&raw.tag),
            capital: (raw.capital != 0).then_some(raw.capital),
            province_count: raw.province_count,
            observer: raw.flags & ANVIL_GAME_COUNTRY_OBSERVER != 0,
            treasury: known(raw.treasury, v, ANVIL_GAME_VALUE_TREASURY),
            stability: known(raw.stability, v, ANVIL_GAME_VALUE_STABILITY),
            prestige: known(raw.prestige, v, ANVIL_GAME_VALUE_PRESTIGE),
            manpower: known(raw.manpower, v, ANVIL_GAME_VALUE_MANPOWER),
            monthly_income: known(raw.monthly_income, v, ANVIL_GAME_VALUE_MONTHLY_INCOME),
        })
    }

    /// The IDs of the provinces a country owns, in the game's order.
    pub fn country_provinces(&self, tag: Tag) -> Result<Vec<i32>> {
        let f = function!(self.table, AnvilGameWorldService, country_provinces)?;
        let text = tag.c_text();
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` IDs.
        read_list(
            0i32,
            || format!("world country_provinces {tag}"),
            |out, capacity, count| unsafe { f(text.as_ptr(), out, capacity, count) },
        )
    }

    /// The ids of the running wars.
    pub fn war_ids(&self) -> Result<Vec<u64>> {
        let f = function!(self.table, AnvilGameWorldService, war_ids)?;
        // SAFETY: the service fills at most `capacity` ids.
        read_list(
            0u64,
            || "world war_ids".to_owned(),
            |out, capacity, count| unsafe { f(out, capacity, count) },
        )
    }

    /// A running war.
    pub fn war(&self, id: u64) -> Result<War> {
        let f = function!(self.table, AnvilGameWorldService, war)?;
        let mut raw: AnvilGameWar = sized();
        // SAFETY: `size` says how much it may write.
        check(unsafe { f(id, &mut raw) }, || format!("world war {id:#x}"))?;
        Ok(War {
            id: raw.id,
            attacker: Tag::from_raw(&raw.attacker),
            defender: Tag::from_raw(&raw.defender),
        })
    }

    /// A war's name as the game shows it.
    pub fn war_name(&self, id: u64) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, war_name)?;
        // SAFETY: the service fills at most `capacity` bytes.
        read_text(
            || format!("world war_name {id:#x}"),
            |buffer, capacity, length| unsafe { f(id, buffer, capacity, length) },
        )
    }

    /// The countries on one side of a war now.
    pub fn war_side(&self, id: u64, side: Side) -> Result<Vec<Tag>> {
        let f = function!(self.table, AnvilGameWorldService, war_side)?;
        let side_raw = match side {
            Side::Attackers => ANVIL_GAME_WAR_ATTACKERS,
            Side::Defenders => ANVIL_GAME_WAR_DEFENDERS,
        };
        // SAFETY: the service fills at most `capacity` tags.
        let raw = read_list(
            EMPTY_TAG,
            || format!("world war_side {id:#x}"),
            |out, capacity, count| unsafe { f(id, side_raw, out, capacity, count) },
        )?;
        Ok(raw.iter().filter_map(Tag::from_raw).collect())
    }

    /// The id of a modifier from its script key (`land_morale`,
    /// `local_tax_modifier`), case included. It holds as long as the game
    /// runs: ask once, as this walks every definition.
    pub fn modifier_id(&self, key: &str) -> Result<ModifierId> {
        let f = function!(self.table, AnvilGameWorldService, modifier_id)?;
        let key_c = crate::text::c_string(key);
        let mut id = 0i32;
        // SAFETY: a NUL-terminated key and an out pointer.
        check(unsafe { f(key_c.as_ptr(), &mut id) }, || {
            format!("world modifier_id {key}")
        })?;
        Ok(ModifierId(id))
    }

    /// A country's modifier as the game uses it (a yes-or-no one is 1000 or 0).
    pub fn country_modifier(&self, tag: Tag, id: ModifierId) -> Result<Milli> {
        let f = function!(self.table, AnvilGameWorldService, country_modifier)?;
        let text = tag.c_text();
        let mut value = 0i32;
        // SAFETY: a NUL-terminated tag and an out pointer.
        check(unsafe { f(text.as_ptr(), id.0, &mut value) }, || {
            format!("world country_modifier {tag} {}", id.0)
        })?;
        Ok(Milli(value))
    }

    /// A province's own modifier (not its owner's values).
    pub fn province_modifier(&self, province: i32, id: ModifierId) -> Result<Milli> {
        let f = function!(self.table, AnvilGameWorldService, province_modifier)?;
        let mut value = 0i32;
        // SAFETY: an out pointer.
        check(unsafe { f(province, id.0, &mut value) }, || {
            format!("world province_modifier {province} {}", id.0)
        })?;
        Ok(Milli(value))
    }

    /// The ids of a country's armies and navies, in the game's order.
    pub fn country_units(&self, tag: Tag) -> Result<Vec<u64>> {
        let f = function!(self.table, AnvilGameWorldService, country_units)?;
        let text = tag.c_text();
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` ids.
        read_list(
            0u64,
            || format!("world country_units {tag}"),
            |out, capacity, count| unsafe { f(text.as_ptr(), out, capacity, count) },
        )
    }

    /// A unit by its country and id.
    pub fn unit(&self, tag: Tag, id: u64) -> Result<Unit> {
        let f = function!(self.table, AnvilGameWorldService, unit)?;
        let text = tag.c_text();
        let mut raw: AnvilGameUnit = sized();
        // SAFETY: a NUL-terminated tag; `size` says how much it may write.
        check(unsafe { f(text.as_ptr(), id, &mut raw) }, || {
            format!("world unit {tag} {id:#x}")
        })?;
        Ok(Unit {
            kind: UnitKind::from_raw(raw.kind),
            id: raw.id,
            country: Tag::from_raw(&raw.country),
            location: (raw.location != 0).then_some(raw.location),
            mission: Mission(raw.mission),
            subunits: raw.subunits,
            strength: Milli(raw.strength),
            leader: (raw.leader != 0).then_some(raw.leader),
        })
    }

    /// The name the game shows for a unit ("1st Army").
    pub fn unit_name(&self, tag: Tag, id: u64) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, unit_name)?;
        let text = tag.c_text();
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` bytes.
        read_text(
            || format!("world unit_name {tag} {id:#x}"),
            |buffer, capacity, length| unsafe { f(text.as_ptr(), id, buffer, capacity, length) },
        )
    }

    /// A unit's leader's name; `NotFound` also for a unit without one.
    pub fn unit_leader_name(&self, tag: Tag, id: u64) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, unit_leader_name)?;
        let text = tag.c_text();
        // SAFETY: as in unit_name.
        read_text(
            || format!("world unit_leader_name {tag} {id:#x}"),
            |buffer, capacity, length| unsafe { f(text.as_ptr(), id, buffer, capacity, length) },
        )
    }

    /// The armies and navies the player has selected, in the game's order.
    pub fn selected_units(&self) -> Result<Vec<UnitRef>> {
        let f = function!(self.table, AnvilGameWorldService, selected_units)?;
        let empty = AnvilGameUnitRef {
            id: 0,
            country: EMPTY_TAG,
            kind: 0,
        };
        // SAFETY: the service fills at most `capacity` entries.
        let raw = read_list(
            empty,
            || "world selected_units".to_owned(),
            |out, capacity, count| unsafe { f(out, capacity, count) },
        )?;
        Ok(raw
            .iter()
            .map(|r| UnitRef {
                id: r.id,
                country: Tag::from_raw(&r.country),
                kind: UnitKind::from_raw(r.kind),
            })
            .collect())
    }
}
