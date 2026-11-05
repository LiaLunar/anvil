# Plugins without addresses: world reads and events, then a panel, changes last

Status: **accepted** (2026-11-05). Nothing here is built yet. The maintainer's four decisions are at the end.

A plugin author without a `symbols.toml` has the host API, `anvil-game` (ready, frame, session, main menu, date, `run_on_main_thread`) and, when the player has Tongs, `tongs.ui` and `tongs.navigation`. They cannot read the world, react to what happens in a campaign, open a window of their own or change the game. This ADR takes those in the order the maintainer set, reads and events first, a window second, changes on the long view, and gives each stage a decision.

How to read the evidence. **[code]**: read in this repository, Tongs or the game's files. **[pdb]**: static, from the GOG build's `eu4.pdb` and its disassembly; addresses in this ADR are GOG's. **[live]**: seen in the game in an earlier lab run. **[measured]**: measured for this ADR. **[hypothesis]**: not checked. No game was started for this ADR.

## Stage 1: reads and events

### Decision

`anvil-game` gets a read-only service `anvil-game.world` version 1 and events for the date, the player's country and, later, wars. Anvil's rule for this is already written: plugins share knowledge of the game through the services of library plugins, not through shared addresses (design §21). It belongs in a standard plugin rather than on Tongs because Tongs has `affects_gameplay = true`, so a plugin that needs it would bring a control pipe to players and a line in the multiplayer hash. Tongs' ADR 0002 already says state any plugin may need belongs in `anvil-game`.

### What v1 exposes

What Tongs reads and has checked in the game [code, live: tongs#107, Tongs design "World reads"], plus the five country values authors will ask for first:

```c
#define ANVIL_GAME_WORLD_SERVICE "anvil-game.world"
#define ANVIL_GAME_WORLD_SERVICE_VERSION 1

typedef struct AnvilGameTag { char text[4]; } AnvilGameTag;  /* "RUS"; "" for no country */

typedef struct AnvilGameProvince {
    uint32_t size;                    /* set by the caller: what it knows; only that much is written */
    int32_t id;
    AnvilGameTag owner, controller;
    int32_t base_tax, base_production, base_manpower;  /* as stored: the value x 1000 */
    uint32_t flags;                   /* ANVIL_GAME_LAND, ANVIL_GAME_WASTELAND */
} AnvilGameProvince;

typedef struct AnvilGameCountry {
    uint32_t size;
    AnvilGameTag tag;
    int32_t capital;                  /* 0: none */
    int32_t province_count;           /* > 0: it exists on the map */
    uint32_t flags;                   /* ANVIL_GAME_OBSERVER */
    int32_t treasury, stability, prestige, manpower;   /* as stored: the value x 1000 */
    int32_t monthly_income;           /* as stored: the value x 1000, as of the last monthly update */
} AnvilGameCountry;

typedef struct AnvilGameWorldService {
    uint32_t size;
    AnvilResult (*player)(AnvilGameTag *out);        /* the country the game treats as the player's */
    int32_t (*province_limit)(void);                 /* valid IDs are 1 .. limit - 1; 0 outside a session */
    AnvilResult (*province)(int32_t id, AnvilGameProvince *out);
    AnvilResult (*province_name)(int32_t id, char *utf8, uint32_t capacity, uint32_t *length);
    AnvilResult (*province_cores)(int32_t id, AnvilGameTag *out, uint32_t capacity, uint32_t *count);
    AnvilResult (*country)(const char *tag, AnvilGameCountry *out);
    AnvilResult (*country_provinces)(const char *tag, int32_t *ids, uint32_t capacity, uint32_t *count);
} AnvilGameWorldService;
```

The buffers are the caller's, so no pointer into game memory leaves the service and ADR 0001's lifetime rule has nothing to cover.

**The five values.** They are `CFixedPoint` fields of `CCountry` [pdb], and each has a code site that gives its offset, in the same bytes on both builds [pdb, and a byte search of the Steam exe]:

| Field | Offset | Symbol's site |
| --- | --- | --- |
| `treasury` | `+0xBB0` | `CCountry::GetTreasury` |
| `stability` | `+0xBD8` | `CCountry::GetStability` |
| `prestige` | `+0xCF8` | `CCountry::GetPrestige` |
| `manpower` | `+0xDB8` | `CCountry::AddManpower` |
| `monthly_income` | `+0xE38` | `lea rdi,[r12+0xE38]` in `CCountry::MonthlyUpdateSerial` |

The service copies the field; it never calls these functions. `monthly_income` is what the monthly update writes, the income total of the economy view before expenses, and not the result of `GetEstimatedMonthlyIncome`, which takes arguments and calculates. The five were compared with the game's top bar and economy view on both builds [live: anvil#219].

**Not in v1.** The selection stays in Tongs: it hangs off the game session, whose layout differs between builds, and only navigation needs it.

### Threads and cost

- **Thread.** Only on the main thread after an outer frame: in a handler of an `anvil-game` event, or in `run_on_main_thread` work. Anywhere else `ANVIL_ERROR_WRONG_PHASE`, and nothing is read. An off-thread reader sees days half applied [live: tongs#107], and a frame drawn inside the tick is also mid-day [code]. This is the rule of `tongs.ui`.
- **Reads.** Each call starts from the root and copies memory under a guard, as Tongs does. No game function is called: a getter may fill a cache, and a cache is state [hypothesis]. Nothing is kept between calls, so a load cannot leave a stale pointer.
- **Results.** `ANVIL_ERROR_NOT_FOUND` for an unknown ID or tag, `ANVIL_ERROR_WRONG_PHASE` outside a session, `ANVIL_ERROR_INTERNAL` for a fault or a failed guard, which costs that answer only, and `ANVIL_ERROR_NOT_DECLARED` for a field absent on this build (below).
- **Cost.** A guarded read took about 2 ns: 21 of them, the count of one `province`, took 43 ns on the lab machine (median of 31 passes in 3 processes) [measured, on ordinary memory]. The whole map is then about 0.2 ms, more with cache misses and names [hypothesis: under 5 ms]. `anvil-game` adds one integer comparison per outer frame, and the game draws 10–17 frames a second at speed 5, nested ones included [live]. So Anvil adds nothing measurable; a consumer pays for what it reads, and the header warns against a whole-map pass every day, since a median day takes 34.8 ms [live]. A live check confirms these before release.

### Compatibility

Within API 2 and ADR 0001: a new service, new events, no new host field. A consumer declares `anvil-game` with the first version that has them, and `get_service` gives NULL on an older one. Fields are appended behind `size`, and a changed meaning is version 2 next to version 1. Because a header's promises are contract, v1 promises little: not that a field exists on every future build, not one `day` per game day, not an order against `anvil-game.frame`.

**Blast radius.** `anvil-game` is the plugin the suspension of gameplay plugins and safe mode depend on, and Anvil skips a plugin when any of its symbols fails. Eighteen more symbols would make every new EU4 version wait for them. The answer is the `absent` key (design §5.2): a port may list a world symbol as absent on a new build, `anvil-game` loads, and the service answers `ANVIL_ERROR_NOT_DECLARED` for that field or NULL when too much is missing. The port of the core never waits for the world.

### Moving Tongs' reads

The maintainer's assumption was that this is mainly interface work. **For provinces, countries and the player that is true; for the events beyond the date it is not.**

- `src/game_world.cpp` (189 lines), its layout and `ReadRaw`/`ReadString` (40 lines) depend on nothing in Tongs but `GameTextToUtf8` and `TagBucket` [code]. Its 13 symbols have tables for both builds [code]: `province_count`, `province_is_sea`, `province_is_wasteland`, `province_base_tax`, `province_base_production`, `province_base_manpower`, `province_controller`, `province_cores`, `province_name`, `player_country`, `tag_buckets`, `country_exists`, `country_provinces`. `anvil-game` already declares the root.
- Five things are not copying. The thread guard, which Tongs' dispatcher gives it for free. Caller-owned strings and lists. A stand-in world for `anvil_game_plugin_tests`, because Tongs' walker has only ever been tested in the game. Eighteen more `bytes` fragments in a file ADR 0003 moves to digests last. And text: `GameTextToUtf8` treats everything as Windows-1252, while Mei registers valid UTF-8 as it is, so a Chinese province name would come out garbled [code]. The service decodes as Mei's `game_text.cpp` does: valid UTF-8 stays, anything else is Windows-1252.
- **Copy, then share.** `anvil-game` gets its own copy; Tongs keeps its reader until its minimum Anvil has the service, then routes `province`, `country` and `player` through it. Tongs still keeps most of the 13 symbols: `NavigationLayout` embeds `WorldLayout` and `game_navigation.cpp` reads and compares the same offsets [code]. Sharing removes a second reader, not Tongs' addresses.

### Events

| Event | Source | New symbols | Determinism and MP | Verdict |
| --- | --- | --- | --- | --- |
| `anvil-game.day`, `.month`, `.year` | the date `anvil-game` already reads, compared after the outer frame | 0 | No code in the tick. Only reads. | v1 |
| `anvil-game.player` | `player()` compared after the outer frame | 0 beyond the world | Local to each machine by nature | v1 |
| `anvil-game.war` (started, ended) | `_CurrentWars` at `root+0x1E28`, `CPdxArray<CWar*>` [pdb], compared by pointer at each `day`; names and sides from `CWar` | about 2 [hypothesis] | No code in the tick. O(wars) a day. | after a check on Steam and in the game |
| The player's event | `_PendingEvents` at `root+0x1E98` compared [pdb], or Tongs' pop-up read | 1–2 [hypothesis] | No code in the tick | open: `CEventManager::TriggerEvent` adds to it [pdb], but under which condition is not read yet |
| Any country's event | a hook of `CEventManager::TriggerEvent`, called from `DailyUpdateSerial` and effects [pdb] | 1 and a change | A callback in the middle of a day, with half-applied state | no |
| Any country's tag | a hook of `CGameState::ChangeTags`, or a pass over all countries | 1 and a change | The same | no |

- **Compare, do not hook.** Every event in v1 is a comparison after an outer frame, so it adds no change to the game and no code to the tick, and it needs no port beyond what `anvil-game` already has. A hook of `HandleDailyTick` is one symbol and exact, but its callback runs while the day is half applied, and every author would have to learn that.
- **One event per frame, with a count.** The payload holds the date, the previous date and `days`. In single player the game posts at most one `CDailyTickCommand` per `CGameState::Tick`, whose only caller is `CInGameIdler::Idle`, and `_bIsInHandleDailyUpdate` gates it [pdb]; an MP client may run several queued days in one frame [hypothesis]. Tongs' `date` notification takes the same shape.
- **What it means.** The date moves inside `HandleDailyTick`, and the monthly and yearly updates run in the same call [pdb, live], so at `month` they are done. No event crosses a session: the first date of a session is the baseline, and a load is a session change.
- **Limits.** Frame timing differs between machines, so these events are local observations: not a clock for anything that must match in MP. A plugin that acts on them through the game's input or memory stops being a reader, and `affects_gameplay` says so. A native plugin can do anything; the service only ensures that using it alone changes nothing.

**Hidden information.** `province` answers for terra incognita, and `country` for any tag, a rival's treasury included. In a multiplayer game that is more than the player sees. Tongs' reads have the same reach, and Anvil cannot stop a native plugin from reading memory, so filtering in the service would be a promise it cannot keep. The header and the author's docs say so, and a plugin that shows such values in multiplayer does not enter the catalog.

## Stage 2: a window in the game

| | The game's own GUI | A panel in the overlay | The plugin draws itself |
| --- | --- | --- | --- |
| The player sees | A real EU4 window: its look, scale, tooltips | An Anvil window over the game, in the overlay's style | Whatever the author makes |
| Works without a mod | No: a `.gui` must be in the game's files, which only a mod adds; `anvil/plugins/<id>` is not one [code] | Yes | Yes |
| Author's cost | `.gui`, `.gfx`, localisation, and click handling in native code | About 30 lines (below) | Own Dear ImGui, D3D9 hooks, `Reset`, input, fonts |
| Anvil's cost | A new library plugin: create, destroy, show, set text, clicks | One service in `anvil-overlay` | None; it works today |
| New addresses at each EU4 version | 15–25 [hypothesis], plus the widget layout | **0** | **0** |
| Checksum | Unchanged: `interface/`, `gfx/` and `localisation/` are not in `checksum_manifest.txt` [code]. `common/custom_gui` is, so the game's scripted buttons change it. Not tried with a plugin's mod; only `anvil/**` is (E1) | Unchanged | Unchanged |
| Multiplayer, ironman | Local, if a click posts nothing | Local | Local |
| Translations | The game's localisation files, and Mei draws CJK | The author's texts, in any language: CJK through the fonts of Windows (below) | The author's |
| Game updates and mods | Most exposed: Tongs needs about 80 symbols to read the tree alone [code]; another mod's window is no conflict, as the name is the plugin's | Only D3D9 | Only D3D9 |
| How it fails | Hard. Campaigns leave about 90 hidden top-level windows; destroying the wrong one, or a popup at the wrong moment, crashed Hamweard's port in run after run (lab `docs/anvil-menu-patch.md`); a stale widget ends the process through `_purecall`, which no guard catches (Tongs ADR 0002) | The overlay is off on OpenGL, where the panel is simply not drawn | Each plugin's hooks and input chain with every other's |

**Recommendation: the panel, as a declarative service.** It is the only one that keeps the promise of this ADR, a plugin with no addresses. It builds on what exists, `anvil-overlay` drawing a settings editor from a schema, and it keeps §15's rule that plugins get no ImGui context: they hand over rows, and the overlay draws them.

```c
#define ANVIL_PANEL_SERVICE "anvil-overlay.panel"

typedef struct AnvilPanelRow {
    uint32_t kind;       /* ANVIL_PANEL_TEXT, ANVIL_PANEL_BUTTON, ANVIL_PANEL_SEPARATOR */
    const char *label;   /* UTF-8 */
    const char *value;   /* UTF-8, may be NULL */
    uint64_t id;         /* given back on a click */
} AnvilPanelRow;

typedef struct AnvilPanelService {
    uint32_t size;
    AnvilResult (*create)(AnvilPlugin *self, const char *title, AnvilPanel **out);
    AnvilResult (*set_rows)(AnvilPanel *panel, const AnvilPanelRow *rows, uint32_t count);  /* copies; any thread */
    AnvilResult (*on_click)(AnvilPanel *panel, void (*click)(void *context, uint64_t id), void *context);
    AnvilResult (*show)(AnvilPanel *panel, int shown);
    const char *(*language)(void);  /* the player's language: "en", "zh-Hans", ... */
} AnvilPanelService;
```

```cpp
// The player's provinces, each month, in a window. No symbols.toml.
void on_month(const char*, const void*, void*) noexcept {
    AnvilGameTag me{};
    AnvilGameCountry c{.size = sizeof c};
    if (world->player(&me) != ANVIL_OK || world->country(me.text, &c) != ANVIL_OK) return;
    const std::string provinces = std::to_string(c.province_count);
    const AnvilPanelRow rows[] = {{ANVIL_PANEL_TEXT, "Country", me.text, 0},
                                  {ANVIL_PANEL_TEXT, "Provinces", provinces.c_str(), 0}};
    panels->set_rows(panel, rows, 2);
}
```

- **A click runs after an outer frame.** The overlay draws in `EndScene`, and frames are also drawn inside the tick (`outer = 0`). So a click is queued through `run_on_main_thread` and the handler may use `anvil-game.world`. Without `anvil-game` there are no clicks.
- **Cost.** `EndScene` only passes on while nothing is shown (§15); a shown panel costs an ImGui frame, which the release measures, and it adds a budget line to §17.
- **Chinese, Japanese and Korean draw.** The overlay's own fonts hold only the characters of Anvil's catalogs (§15), so a plugin's text needs more than they have. Shipping full fonts is not needed: Windows 10 and 11 list Microsoft YaHei, Microsoft JhengHei, Yu Gothic and Malgun Gothic in their base fonts, outside the optional language features [Microsoft's font lists, and present on the lab's pl-PL machine], and the overlay already loads Segoe UI from that folder [code].
  - The panel merges the font of the player's language after Latin; with a Latin language, Hangul takes Malgun Gothic, kana Yu Gothic and Han YaHei.
  - It needs Dear ImGui 1.92 or later, which draws glyphs on demand and grows the atlas, with no list of ranges, and whose Direct3D 9 backend supports it [ImGui's `FONTS.md` and `imgui_impl_dx9.cpp` at v1.93.0]. Anvil pins v1.91.8, so the upgrade is a pull request of its own, for the overlay and the Manager, which also drops `ClearTexData` after the upload. Without it, each new character means a full atlas rebuild in `EndScene`.
  - A font that is missing (a stripped Windows image) shows `?` for those characters and writes one log line. Nothing is downloaded, and no font ships, so the package and the licences do not change.
  - The texts stay the author's: Anvil translates none of them. The service gives the player's language, as `anvil.control` gives the overlay.
- **Text input** is not in v1: it needs focus rules with the game's keyboard.
- **The game's GUI stays open.** A plugin with a mod can get a real window later, through a library plugin that takes the lifetime problem on once. Hamweard and Etch have paid for that knowledge; this ADR does not plan it.
- **Own drawing** stays possible and is not promoted. Two plugins doing it fight over the window procedure and the device.

## Stage 3: changes in the game

An outline, no API. Changing the game is possible today for a plugin that declares it; what is missing is a way to do it that is safe. It is safe only when all of this is true.

1. **It goes through a game command.** The simulation is command-driven: `CSession::Post`, and every machine runs what comes back, down to `daily_tick` [pdb, live]. An AI decision reaches MP clients as commands [live]. A plugin that posts a command a player could post should need no twin on the other machines [hypothesis: no plugin has done it]. Anything else needs the same plugin, version and timing everywhere.
2. **Never from a frame.** Frames differ between machines. A direct write can only happen inside something every machine runs at the same point: a command or the tick. So the events of stage 1 are never a trigger for it.
3. **No extra draw from the game's RNG.** One extra draw on one machine gave an out-of-sync in 2 game days [live: `lab-oos`].
4. **The game can check it.** After each `CDailyTickCommand` the game compares checksums of provinces and countries, 140 sums with the RNG count [pdb, live]. What no sum covers can drift unnoticed. A change needs a list of what it touches and a test in MP, with the detector's sensitivity shown, as `lab-oos` did.
5. **`affects_gameplay` stays the gate, and the core enforces it.** Today a service cannot see who calls it (Tongs ADR 0002). `get_service` has `self`, so the core could refuse a writing service to a plugin with `affects_gameplay = false`, and refuse calls while it is suspended, as `TONGS_NAV_SUSPENDED` does.
6. **Ironman stays the player's opt-in, and it is a question of fairness, not of code.** The game's own gate sees only the checksum (`_bGameOK`), so a plugin is invisible to it [pdb]. Only Anvil's rule stands between a change and an achievement. §13 stays: off by default, opt-in, a permanent notice.
7. **Multiplayer needs a way to agree.** The hash is compared by hand, and an automatic exchange is not planned because a channel in the netcode risks desyncs (§21). For command-only changes (1) that is enough. For any other it is not, so they stay out of multiplayer.
8. **The change survives saving.** Nothing a plugin adds goes into a save. State beyond the game's own fields needs a sidecar keyed to the campaign, and a hot-joining player does not get it.

So the first useful step, when it comes, is a small service that posts a short list of existing commands, for plugins with `affects_gameplay = true`. Everything else stays what it is today, a hook at the author's own risk.

## What each stage gives

| | 1a. Reads, date, player | 1b. War | 2. Panel | 3. Changes |
| --- | --- | --- | --- | --- |
| For the author | The world and a country's numbers, with no reverse engineering | The first event about the campaign itself | A visible result with no drawing code, in any language | Effects, within rules |
| For the player | Loggers, exporters and reminders | Alerts | One place for plugin windows | New mechanics |
| Unlocks | Stage 2's example, campaign statistics, Tongs on one reader | War logs | Plugins with no mod | Gameplay plugins that keep MP |
| Risk | Low. Reads only; a wrong read costs an answer. The five values are not yet checked live | Low to medium: Steam's offset unchecked | Low; the ImGui upgrade touches the Manager too | High |
| Effort [hypothesis] | One release, a stand-in world, one live check | One PR and a live check | The ImGui upgrade, one release and a budget line | Research first; unknown |
| New symbols, so addresses at each new EU4 version | 18 (`absent` when a port needs it) | About 2 | 0 | Unknown; at least 2, plus each command's layout |

## Considered options

- *Build on Tongs' services.* Rejected: a consumer would require a plugin with `affects_gameplay = true` and a control pipe, and players in MP would see it in the hash.
- *A new standard plugin, `anvil-world`.* Keeps `anvil-game` at 16 symbols and fails on its own, at the cost of a third DLL and a service name apart from the rest of the game's knowledge. `absent` gives the same safety within one plugin. Rejected by the maintainer.
- *Full CJK fonts in the package.* Source Han Sans is many megabytes per language, for what Windows already has. Rejected.
- *Reads of the country values through the game's getters.* Rejected: `GetEstimatedMonthlyIncome` calculates, and a game call from a plugin is what this service exists to avoid.
- *A snapshot of the world each day, readable from any thread.* Rejected: it costs every player what few plugins use, and it decides for authors which fields matter.
- *Publishing each game event through one generic stream.* Rejected for now: nothing needs it, and a contract for events we have not read is a promise we could not keep.
- *Events from the game's logs.* Rejected: free text, in the player's language, and throttled by `anvil-log-throttle`.

## Consequences

- `anvil-game` grows from 16 to 34 symbols, and `tests/anvil_game_plugin_tests.cpp` needs a stand-in world.
- The recipes on anvil.happydwarf.dev follow the first release: a plugin with no addresses can now read the campaign.
- `design.md` §4, §13.1 and §15 are updated by the pull request that builds each stage, not by this ADR.
- Tongs' pipe does not change. Its ADR 0002 keeps `tongs.events` for what the player's windows show.

## Limits

- No game was started for this ADR; the release of stage 1a was checked in the game on both builds (anvil#219). The Steam offsets of `root+0x1E28` and `root+0x1E98` and the `_PendingEvents` condition are unchecked; `root+0x1DD0` and `root+0x1E60` from the same class do match Steam [code].
- System fonts in the panel are not tried: neither a CJK panel in the game nor the ImGui upgrade.
- Whether anything of the simulation runs between outer frames is open: the lab's single-player AI hypothesis says decisions may spread across frames (lab `docs/range-witness-verify.md`). Reads every frame at speed 5 gave no failed read in about 139 000 reads on both builds [live: anvil#219]; that shows the reads hold, not that nothing runs.
- The number of symbols for the game's GUI, and every effort in the table, are estimates.

## Decisions of the maintainer (2026-11-05)

1. **Where it lives:** `anvil-game.world`, with `absent` as the valve. Not a separate `anvil-world`.
2. **How far v1 goes:** Tongs' set and, at once, treasury, stability, prestige, manpower and income: 18 symbols, with a live check of the five.
3. **The panel draws plugin text in Chinese, Japanese and Korean.** The first draft of this ADR accepted that it would not. The maintainer asked why not, and the system fonts above are the answer.
4. **Hidden information:** documented and kept out of the catalog, not filtered in the service: a filter promises what Anvil cannot enforce.
