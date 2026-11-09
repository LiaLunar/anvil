//! A country panel in Rust: the player's country, read from the world service
//! (`anvil-game.world`) at the start of a session, every month and on a
//! click, and shown in a panel of Anvil's overlay (`anvil-overlay.panel`).
//!
//! It has no game addresses and calls no game code: everything goes through
//! the safe `anvil` crate, without `unsafe`. It changes nothing in the game,
//! so `affects_gameplay = false`.
#![forbid(unsafe_code)]

use std::fmt::Write as _;
use std::sync::{Arc, Mutex};

use anvil::game::{self, Game, Session, SessionChange};
use anvil::panel::{Panel, Panels, Row};
use anvil::world::{UnitKind, World};
use anvil::{Error, ErrorCode, Host, Plugin, Tag};

const REFRESH: u64 = 1;
const SELECTED_UNITS: u64 = 2;

/// What the panel shows, rebuilt from the world on each refresh.
struct View {
    host: Host,
    game: Game,
    world: Option<World>,
    panel: Option<Panel>,
    /// The last summary, so a refresh with nothing new does not log again.
    last: Mutex<String>,
}

/// One refresh: the rows and the line for the log.
struct Report {
    rows: Vec<(String, String)>,
    summary: String,
}

impl View {
    fn show(&self, rows: &[Row<'_>]) {
        if let Some(panel) = self.panel {
            if let Err(error) = panel.set_rows(rows) {
                self.host.warn(format_args!("{error}"));
            }
        }
    }

    fn show_message(&self, message: &str) {
        self.show(&[Row::text(message)]);
    }

    /// Reads the player's country. Runs on the game's main thread: from an
    /// anvil-game event or a panel click.
    fn read(&self) -> anvil::Result<Report> {
        let world = self.world.ok_or_else(|| {
            Error::new(
                ErrorCode::Unsupported,
                "no world service (anvil-game 1.11 or later)",
            )
        })?;
        let tag = world
            .player()?
            .ok_or_else(|| Error::new(ErrorCode::NotFound, "the player has no country"))?;
        let country = world.country(tag)?;
        let capital = match country.capital {
            Some(id) => format!("{} ({id})", world.province_name(id)?),
            None => "none".to_owned(),
        };
        let date = self
            .game
            .date()
            .map_or_else(|| "?".to_owned(), |d| d.to_string());
        let shown = |value: Option<anvil::world::Milli>| {
            value.map_or_else(|| "?".to_owned(), |v| v.to_string())
        };
        let rows = vec![
            ("Country".to_owned(), tag.to_string()),
            ("Date".to_owned(), date.clone()),
            ("Capital".to_owned(), capital.clone()),
            ("Provinces".to_owned(), country.province_count.to_string()),
            ("Treasury".to_owned(), shown(country.treasury)),
            ("Monthly income".to_owned(), shown(country.monthly_income)),
            ("Stability".to_owned(), shown(country.stability)),
            ("Prestige".to_owned(), shown(country.prestige)),
            (
                "Manpower".to_owned(),
                country
                    .manpower
                    .map_or_else(|| "?".to_owned(), |m| format!("{m}k")),
            ),
        ];
        let summary = format!(
            "{tag} on {date}: capital {capital}, {} provinces, treasury {}, stability {}",
            country.province_count,
            shown(country.treasury),
            shown(country.stability)
        );
        Ok(Report { rows, summary })
    }

    fn refresh(&self, why: &str) {
        match self.read() {
            Ok(report) => {
                let mut rows: Vec<Row<'_>> = report
                    .rows
                    .iter()
                    .map(|(label, value)| Row::value(label, value))
                    .collect();
                rows.push(Row::Separator);
                rows.push(Row::button("Refresh", REFRESH));
                rows.push(Row::button("Log the selected units", SELECTED_UNITS));
                self.show(&rows);
                let mut last = self.last.lock().unwrap_or_else(|e| e.into_inner());
                if *last != report.summary {
                    self.host.info(format_args!("{why}: {}", report.summary));
                    *last = report.summary;
                }
            }
            Err(error) => {
                self.host
                    .warn(format_args!("{why}: cannot read the country: {error}"));
                self.show(&[
                    Row::value("Cannot read the country", &error.to_string()),
                    Row::button("Refresh", REFRESH),
                ]);
            }
        }
    }

    fn session(&self, session: &Session) {
        match session.change {
            SessionChange::Started => self.refresh(&format!("session {}", session.number)),
            SessionChange::Ended => {
                self.last.lock().unwrap_or_else(|e| e.into_inner()).clear();
                self.show_message("No game is loaded.");
            }
            SessionChange::Other(_) => {}
        }
    }

    fn log_selected_units(&self) {
        let Some(world) = self.world else { return };
        let units = match world.selected_units() {
            Ok(units) => units,
            Err(error) => {
                self.host.warn(format_args!("selected units: {error}"));
                return;
            }
        };
        if units.is_empty() {
            self.host.info("selected units: none");
            return;
        }
        for unit in units {
            let Some(tag) = unit.country else { continue };
            let mut line = format!(
                "selected: {}",
                world
                    .unit_name(tag, unit.id)
                    .unwrap_or_else(|e| e.to_string())
            );
            if let Ok(details) = world.unit(tag, unit.id) {
                let kind = match details.kind {
                    UnitKind::Army => "army",
                    UnitKind::Navy => "navy",
                    UnitKind::Other(_) => "unit",
                };
                let _ = write!(
                    line,
                    " ({tag} {kind}, {} subunits, strength {}",
                    details.subunits, details.strength
                );
                if let Some(at) = details.location {
                    let _ = write!(
                        line,
                        ", at {}",
                        world.province_name(at).unwrap_or_else(|_| at.to_string())
                    );
                }
                line.push(')');
            }
            self.host.info(line);
        }
    }

    fn click(&self, id: u64) {
        match id {
            REFRESH => {
                self.last.lock().unwrap_or_else(|e| e.into_inner()).clear();
                self.refresh("refresh");
            }
            SELECTED_UNITS => self.log_selected_units(),
            _ => {}
        }
    }
}

struct CountryPanel;

impl Plugin for CountryPanel {
    fn load(host: &mut Host) -> anvil::Result<Self> {
        let game = Game::get(host)
            .ok_or_else(|| Error::new(ErrorCode::Unsupported, "anvil-game is not loaded"))?;
        let world = World::get(host);
        if world.is_none() {
            host.warn("no world service: the panel shows no country");
        }
        let panel = match Panels::get(host) {
            Some(panels) => Some(panels.create("Country (Rust)")?),
            None => {
                host.info("no overlay: the country goes to the log only");
                None
            }
        };
        let view = Arc::new(View {
            host: *host,
            game,
            world,
            panel,
            last: Mutex::new(String::new()),
        });
        view.show_message("No game is loaded.");
        if let Some(panel) = panel {
            let v = Arc::clone(&view);
            panel.on_click(move |id| v.click(id))?;
            panel.show(true)?;
        }
        let v = Arc::clone(&view);
        host.subscribe(game::SESSION, move |session| v.session(session))?;
        let v = Arc::clone(&view);
        host.subscribe(game::MONTH, move |_| v.refresh("month"))?;
        let v = Arc::clone(&view);
        host.subscribe(game::PLAYER, move |change| {
            let tag = change.tag.as_ref().map_or("none", Tag::as_str);
            v.refresh(&format!("now playing {tag}"));
        })?;
        host.info(format_args!(
            "country panel loaded (world service: {}, panel: {})",
            if world.is_some() { "yes" } else { "no" },
            if panel.is_some() { "yes" } else { "no" }
        ));
        Ok(CountryPanel)
    }
}

anvil::plugin!(CountryPanel);
