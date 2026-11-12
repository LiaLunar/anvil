//! `anvil-game.world`: the world of the running session, read straight from
//! the game (anvil-game 1.11.0; modifiers, units and the selection 1.15.0;
//! rulers, heirs, consorts, flags and variables 1.20.0; pending events and
//! statistics 1.21.0).
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
use crate::game::Date;
use crate::host::Host;
use crate::sys::{
    AnvilGameCountry, AnvilGameDate, AnvilGameFlag, AnvilGameMonarch, AnvilGamePendingEvent,
    AnvilGameProvince, AnvilGameStatisticPoint, AnvilGameTag, AnvilGameUnit, AnvilGameUnitRef,
    AnvilGameWar, AnvilGameWorldService, ANVIL_GAME_COUNTRY_OBSERVER, ANVIL_GAME_MONARCH_CONSORT,
    ANVIL_GAME_MONARCH_FEMALE, ANVIL_GAME_MONARCH_HEIR, ANVIL_GAME_MONARCH_REGENCY,
    ANVIL_GAME_MONARCH_RULER, ANVIL_GAME_PENDING_HIDDEN, ANVIL_GAME_PROVINCE_LAND,
    ANVIL_GAME_PROVINCE_WASTELAND, ANVIL_GAME_STATISTIC_INCOME, ANVIL_GAME_STATISTIC_INFLATION,
    ANVIL_GAME_STATISTIC_NATION_SIZE, ANVIL_GAME_STATISTIC_SCORE, ANVIL_GAME_UNIT_ARMY,
    ANVIL_GAME_UNIT_NAVY, ANVIL_GAME_VALUE_MANPOWER, ANVIL_GAME_VALUE_MONTHLY_INCOME,
    ANVIL_GAME_VALUE_PRESTIGE, ANVIL_GAME_VALUE_STABILITY, ANVIL_GAME_VALUE_TREASURY,
    ANVIL_GAME_WAR_ATTACKERS, ANVIL_GAME_WAR_DEFENDERS, ANVIL_GAME_WORLD_SERVICE_VERSION,
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

/// A country's ruler, heir or consort (anvil-game 1.20.0).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MonarchKind {
    /// The ruler.
    Ruler,
    /// The heir.
    Heir,
    /// The consort.
    Consort,
}

impl MonarchKind {
    fn raw(self) -> u32 {
        match self {
            Self::Ruler => ANVIL_GAME_MONARCH_RULER,
            Self::Heir => ANVIL_GAME_MONARCH_HEIR,
            Self::Consort => ANVIL_GAME_MONARCH_CONSORT,
        }
    }

    fn from_raw(kind: u32) -> Self {
        match kind {
            ANVIL_GAME_MONARCH_HEIR => Self::Heir,
            ANVIL_GAME_MONARCH_CONSORT => Self::Consort,
            _ => Self::Ruler,
        }
    }
}

/// A ruler, heir or consort, as the game stores them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Monarch {
    /// Ruler, heir or consort.
    pub kind: MonarchKind,
    /// The game's id of the person.
    pub id: u64,
    /// The country asked about.
    pub country: Option<Tag>,
    /// Female.
    pub female: bool,
    /// A regency council or a regent rules for them.
    pub regency: bool,
    /// Administrative skill, without modifiers.
    pub adm: i32,
    /// Diplomatic skill.
    pub dip: i32,
    /// Military skill.
    pub mil: i32,
    /// An heir's claim strength; 0 for a ruler or a consort.
    pub claim: Milli,
    /// The birth date.
    pub birth: Date,
    /// The id of the leader they also are ([`Unit::leader`]); `None` for none.
    pub leader: Option<u64>,
}

/// The id of a flag, from its name ([`World::flag_id`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FlagId(pub i32);

/// A flag that is set, and when it was set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Flag {
    /// Its id; [`World::flag_name`] gives the name.
    pub id: FlagId,
    /// When it was set.
    pub date: Date,
}

/// An event waiting for a country's choice (anvil-game 1.21.0).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PendingEvent {
    /// The id [`commands`](crate::commands)' event option takes; unique while it waits.
    pub pending_id: i32,
    /// The country it waits for, by the game's rule; `None` for none.
    pub country: Option<Tag>,
    /// The scope's province; `None` for a country event.
    pub province: Option<i32>,
    /// The event's options in its file; the window may disable some.
    pub options: u32,
    /// Hidden: the game shows no window for it.
    pub hidden: bool,
    /// The game chooses for the player the day after this date.
    pub auto_select: Date,
}

/// One of the four series of the game's graphs (anvil-game 1.21.0).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum StatisticKind {
    /// The income graph, whole ducats (the game's value ÷ 1000, truncated).
    Income,
    /// The score, whole points (÷ 1000, truncated).
    Score,
    /// The provinces the country owns, a count.
    NationSize,
    /// Inflation, whole percent (÷ 1000, truncated).
    Inflation,
}

impl StatisticKind {
    fn raw(self) -> u32 {
        match self {
            Self::Income => ANVIL_GAME_STATISTIC_INCOME,
            Self::Score => ANVIL_GAME_STATISTIC_SCORE,
            Self::NationSize => ANVIL_GAME_STATISTIC_NATION_SIZE,
            Self::Inflation => ANVIL_GAME_STATISTIC_INFLATION,
        }
    }
}

/// One year of a series. The value is whole units, not × 1000.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct StatisticPoint {
    /// The year whose 1 January wrote it: 1720 holds the value at the end of 1719.
    pub year: i32,
    /// The value then (0 for a year before the country had one).
    pub value: i32,
}

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

    /// A country's ruler, heir or consort. `NotFound` when it has none: the
    /// game then holds a placeholder that every such country shares, and the
    /// service never reads it. A country no longer on the map keeps its last
    /// ruler: check [`Country::province_count`] when that matters.
    pub fn monarch(&self, tag: Tag, kind: MonarchKind) -> Result<Monarch> {
        let f = function!(self.table, AnvilGameWorldService, monarch)?;
        let text = tag.c_text();
        let mut raw: AnvilGameMonarch = sized();
        // SAFETY: a NUL-terminated tag; `size` says how much it may write.
        check(unsafe { f(text.as_ptr(), kind.raw(), &mut raw) }, || {
            format!("world monarch {tag} {kind:?}")
        })?;
        Ok(Monarch {
            kind: MonarchKind::from_raw(raw.kind),
            id: raw.id,
            country: Tag::from_raw(&raw.country),
            female: raw.flags & ANVIL_GAME_MONARCH_FEMALE != 0,
            regency: raw.flags & ANVIL_GAME_MONARCH_REGENCY != 0,
            adm: raw.adm,
            dip: raw.dip,
            mil: raw.mil,
            claim: Milli(raw.claim),
            birth: Date::from_raw(&raw.birth),
            leader: (raw.leader != 0).then_some(raw.leader),
        })
    }

    /// Their name as the game keeps it; empty in an interregnum.
    pub fn monarch_name(&self, tag: Tag, kind: MonarchKind) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, monarch_name)?;
        let text = tag.c_text();
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` bytes.
        read_text(
            || format!("world monarch_name {tag} {kind:?}"),
            |buffer, capacity, length| unsafe {
                f(text.as_ptr(), kind.raw(), buffer, capacity, length)
            },
        )
    }

    /// Their dynasty as the game shows it (a regency council shows the heir's).
    pub fn monarch_dynasty(&self, tag: Tag, kind: MonarchKind) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, monarch_dynasty)?;
        let text = tag.c_text();
        // SAFETY: as in monarch_name.
        read_text(
            || format!("world monarch_dynasty {tag} {kind:?}"),
            |buffer, capacity, length| unsafe {
                f(text.as_ptr(), kind.raw(), buffer, capacity, length)
            },
        )
    }

    /// The id of a flag from its name, case included. It holds as long as the
    /// game runs: ask once. `NotFound` for a name nothing has set yet in this
    /// run of the game.
    pub fn flag_id(&self, name: &str) -> Result<FlagId> {
        let f = function!(self.table, AnvilGameWorldService, flag_id)?;
        let name_c = crate::text::c_string(name);
        let mut id = 0i32;
        // SAFETY: a NUL-terminated name and an out pointer.
        check(unsafe { f(name_c.as_ptr(), &mut id) }, || {
            format!("world flag_id {name}")
        })?;
        Ok(FlagId(id))
    }

    /// A flag's name.
    pub fn flag_name(&self, id: FlagId) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, flag_name)?;
        // SAFETY: the service fills at most `capacity` bytes.
        read_text(
            || format!("world flag_name {}", id.0),
            |buffer, capacity, length| unsafe { f(id.0, buffer, capacity, length) },
        )
    }

    /// A country's flags, with the date each was set.
    pub fn country_flags(&self, tag: Tag) -> Result<Vec<Flag>> {
        let f = function!(self.table, AnvilGameWorldService, country_flags)?;
        let text = tag.c_text();
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` flags.
        flags(
            || format!("world country_flags {tag}"),
            |out, capacity, count| unsafe { f(text.as_ptr(), out, capacity, count) },
        )
    }

    /// A province's flags as `has_province_flag` sees them, its permanent ones included.
    pub fn province_flags(&self, province: i32) -> Result<Vec<Flag>> {
        let f = function!(self.table, AnvilGameWorldService, province_flags)?;
        // SAFETY: the service fills at most `capacity` flags.
        flags(
            || format!("world province_flags {province}"),
            |out, capacity, count| unsafe { f(province, out, capacity, count) },
        )
    }

    /// A ruler's, heir's or consort's flags; `NotFound` as for [`World::monarch`].
    pub fn monarch_flags(&self, tag: Tag, kind: MonarchKind) -> Result<Vec<Flag>> {
        let f = function!(self.table, AnvilGameWorldService, monarch_flags)?;
        let text = tag.c_text();
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` flags.
        flags(
            || format!("world monarch_flags {tag} {kind:?}"),
            |out, capacity, count| unsafe { f(text.as_ptr(), kind.raw(), out, capacity, count) },
        )
    }

    /// The global flags.
    pub fn global_flags(&self) -> Result<Vec<Flag>> {
        let f = function!(self.table, AnvilGameWorldService, global_flags)?;
        // SAFETY: the service fills at most `capacity` flags.
        flags(
            || "world global_flags".to_owned(),
            |out, capacity, count| unsafe { f(out, capacity, count) },
        )
    }

    /// A country's variable by its name, case included. `NotFound` when it
    /// has none of that name (the game's triggers take that as 0).
    pub fn country_variable(&self, tag: Tag, name: &str) -> Result<Milli> {
        let f = function!(self.table, AnvilGameWorldService, country_variable)?;
        let text = tag.c_text();
        let name_c = crate::text::c_string(name);
        let mut value = 0i32;
        // SAFETY: a NUL-terminated tag and name, and an out pointer.
        let result = unsafe { f(text.as_ptr(), name_c.as_ptr(), &mut value) };
        check(result, || format!("world country_variable {tag} {name}"))?;
        Ok(Milli(value))
    }

    /// A province's variable by its name.
    pub fn province_variable(&self, province: i32, name: &str) -> Result<Milli> {
        let f = function!(self.table, AnvilGameWorldService, province_variable)?;
        let name_c = crate::text::c_string(name);
        let mut value = 0i32;
        // SAFETY: a NUL-terminated name and an out pointer.
        check(unsafe { f(province, name_c.as_ptr(), &mut value) }, || {
            format!("world province_variable {province} {name}")
        })?;
        Ok(Milli(value))
    }

    /// The names of a country's variables, in the game's order.
    pub fn country_variable_names(&self, tag: Tag) -> Result<Vec<String>> {
        let f = function!(self.table, AnvilGameWorldService, country_variable_names)?;
        let text = tag.c_text();
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` bytes.
        let all = read_text(
            || format!("world country_variable_names {tag}"),
            |buffer, capacity, length| unsafe { f(text.as_ptr(), buffer, capacity, length) },
        )?;
        Ok(split_names(&all))
    }

    /// The names of a province's variables, in the game's order.
    pub fn province_variable_names(&self, province: i32) -> Result<Vec<String>> {
        let f = function!(self.table, AnvilGameWorldService, province_variable_names)?;
        // SAFETY: the service fills at most `capacity` bytes.
        let all = read_text(
            || format!("world province_variable_names {province}"),
            |buffer, capacity, length| unsafe { f(province, buffer, capacity, length) },
        )?;
        Ok(split_names(&all))
    }

    /// The ids of the events waiting for a country's choice, in the game's
    /// order; `None` for the player's own country (`NotFound` in observe
    /// mode). An entry counts once it was in the game's list at two outer
    /// frames in a row, which leaves out an AI's choices and hidden events.
    /// Key on the id: the same event can wait twice.
    pub fn pending_events(&self, tag: Option<Tag>) -> Result<Vec<i32>> {
        let f = function!(self.table, AnvilGameWorldService, pending_events)?;
        let text = tag.map(|t| t.c_text());
        let tag_ptr = text.as_ref().map_or(std::ptr::null(), |t| t.as_ptr());
        // SAFETY: a NUL-terminated tag or null; the service fills at most `capacity` ids.
        read_list(
            0i32,
            || format!("world pending_events {tag:?}"),
            |out, capacity, count| unsafe { f(tag_ptr, out, capacity, count) },
        )
    }

    /// A waiting event; `NotFound` for an id that is not waiting (or not yet counted).
    pub fn pending_event(&self, pending_id: i32) -> Result<PendingEvent> {
        let f = function!(self.table, AnvilGameWorldService, pending_event)?;
        let mut raw: AnvilGamePendingEvent = sized();
        // SAFETY: `size` says how much it may write.
        check(unsafe { f(pending_id, &mut raw) }, || {
            format!("world pending_event {pending_id}")
        })?;
        Ok(PendingEvent {
            pending_id: raw.pending_id,
            country: Tag::from_raw(&raw.country),
            province: (raw.province != 0).then_some(raw.province),
            options: raw.options,
            hidden: raw.flags & ANVIL_GAME_PENDING_HIDDEN != 0,
            auto_select: Date::from_raw(&raw.auto_select),
        })
    }

    /// The waiting event's key as the game's files name it (`flavor_tur.203`).
    pub fn pending_event_key(&self, pending_id: i32) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, pending_event_key)?;
        // SAFETY: the service fills at most `capacity` bytes.
        read_text(
            || format!("world pending_event_key {pending_id}"),
            |buffer, capacity, length| unsafe { f(pending_id, buffer, capacity, length) },
        )
    }

    /// The localisation key of its title, not the translated text.
    pub fn pending_event_title(&self, pending_id: i32) -> Result<String> {
        let f = function!(self.table, AnvilGameWorldService, pending_event_title)?;
        // SAFETY: the service fills at most `capacity` bytes.
        read_text(
            || format!("world pending_event_title {pending_id}"),
            |buffer, capacity, length| unsafe { f(pending_id, buffer, capacity, length) },
        )
    }

    /// A country's yearly series of the game's graphs, oldest first. It
    /// changes only at [`game::YEAR`](crate::game::YEAR). `NotFound` for a tag
    /// without a series (none has one before its first year end).
    pub fn country_statistics(&self, tag: Tag, kind: StatisticKind) -> Result<Vec<StatisticPoint>> {
        let f = function!(self.table, AnvilGameWorldService, country_statistics)?;
        let text = tag.c_text();
        let empty = AnvilGameStatisticPoint { year: 0, value: 0 };
        // SAFETY: a NUL-terminated tag; the service fills at most `capacity` points.
        let raw = read_list(
            empty,
            || format!("world country_statistics {tag} {kind:?}"),
            |out, capacity, count| unsafe { f(text.as_ptr(), kind.raw(), out, capacity, count) },
        )?;
        Ok(raw
            .iter()
            .map(|p| StatisticPoint {
                year: p.year,
                value: p.value,
            })
            .collect())
    }
}

/// Names that each end with a NUL, one after the other.
fn split_names(all: &str) -> Vec<String> {
    all.split_terminator('\0').map(str::to_owned).collect()
}

fn flags(
    context: impl Fn() -> String,
    call: impl FnMut(*mut AnvilGameFlag, u32, *mut u32) -> crate::sys::AnvilResult,
) -> Result<Vec<Flag>> {
    let empty = AnvilGameFlag {
        id: 0,
        reserved: 0,
        date: AnvilGameDate {
            size: 0,
            year: 0,
            month: 0,
            day: 0,
            session: 0,
        },
    };
    let raw = read_list(empty, context, call)?;
    Ok(raw
        .iter()
        .map(|f| Flag {
            id: FlagId(f.id),
            date: Date::from_raw(&f.date),
        })
        .collect())
}
