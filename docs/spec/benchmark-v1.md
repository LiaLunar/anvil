# Start-up benchmark v1 specification

The start-up benchmark measures, on the player's own computer, how long Europa
Universalis IV 1.37.5 takes to start without a plugin and with it. Anvil
Manager runs a series of starts that quit by themselves, shows the result, and
lets the player send one anonymous report to the maintainers or copy it.
Bellows is its first user.

This document says what v1 contains and in what order it ships. It collects
the decisions of the
[wayfinder map](https://github.com/happy-dwarf-team/anvil/issues/127) in one
place; each section links the ticket that decided it, where the reasons and
the evidence are. Terms are those of [GLOSSARY.md](../../GLOSSARY.md). The
word "benchmark" alone names Anvil's microbenchmarks (`anvil_bench`, design
§17); this feature is the **start-up benchmark**.

| Decision | Where it lives |
| --- | --- |
| How a start ends itself and how its time is learned | [anvil#128](https://github.com/happy-dwarf-team/anvil/issues/128) ([research](https://github.com/happy-dwarf-team/anvil/blob/research/benchmark-start-end/docs/research/benchmark-start-end.md)) |
| Cold starts without administrator rights | [anvil#129](https://github.com/happy-dwarf-team/anvil/issues/129) ([research](https://github.com/happy-dwarf-team/anvil/blob/research/benchmark-cold-start/docs/research/benchmark-cold-start.md)) |
| What a report contains | [anvil#130](https://github.com/happy-dwarf-team/anvil/issues/130) |
| How a series runs | [anvil#131](https://github.com/happy-dwarf-team/anvil/issues/131) |
| Where reports land | [anvil#132](https://github.com/happy-dwarf-team/anvil/issues/132) |
| What the player sees | [anvil#133](https://github.com/happy-dwarf-team/anvil/issues/133) ([prototype](https://claude.ai/artifact/B6geL77yKGV1VAE3xr6VV1), private) |
| A clean quit, tried live | [anvil#134](https://github.com/happy-dwarf-team/anvil/issues/134) ([prototype](https://github.com/happy-dwarf-team/anvil/tree/prototype/benchmark-quit/prototypes/bench-quit)) |
| Plugin fields: `anvil.benchmark` v1 | [anvil#135](https://github.com/happy-dwarf-team/anvil/issues/135) |
| Releases of Anvil and Bellows | [anvil#136](https://github.com/happy-dwarf-team/anvil/issues/136) |

Until the Anvil release that shows the Benchmark tab, this specification is a
living document: when implementation learns something that changes it, a pull
request changes it. After that release it is frozen, and the next version gets
its own file.

## 1. Purpose and principles

- **A diagnostic signal first.** The data is for finding setups where a plugin
  does not help or slows the start (an HDD, many mods, little memory). Showing
  the player their own result is what makes them take part. A public page of
  results is not part of v1.
- **Generic in Anvil.** The series, the UI, the report and the receiver are
  Anvil's, for "plugin X on / off". Bellows is the first compared plugin and
  adds its own fields through `anvil.benchmark` v1 (section 5).
- **Two arms only.** *Without* the plugin means Anvil and `anvil-game` with the
  compared plugin not loaded, as in the Bellows README table. There is no
  vanilla arm without Anvil: removing Anvil needs an uninstall, and Anvil alone
  does not change the start time.
- **No administrator rights.** No UAC prompt and no system cache flush. Cold
  starts cannot be made on demand (anvil#129); v1 shows the cold starts a warm
  series happens to contain.
- **Nothing leaves without a click.** A report goes out only through an
  explicit **Send**, after the player has seen exactly the bytes that go out,
  anonymously, with **Copy** as the alternative. This is a deliberate exception
  to Anvil's "nothing is uploaded" rule, limited to that click.
- **The player's normal start.** The Manager changes neither the game's
  priority nor its affinity, and never writes `anvil.toml`, saves or mods.
- **Never kill on purpose.** A kill leaves `refreshRate` reset in the player's
  `settings.txt` until the next clean exit (anvil#134).

## 2. A benchmark start

### Starting

- The Manager starts `eu4.exe` directly from the game folder (not through
  `steam://`, which opens the Paradox launcher and needs a Play click), with
  Steam running and `SteamAppId=236850` in the child's environment. Without
  either, the game starts without the Steam client and overlay, which is not
  the player's usual start (anvil#128, anvil#134).
- The **arm** is passed in the start's environment as
  `ANVIL_BENCHMARK=without:<plugin id>` or `ANVIL_BENCHMARK=with:<plugin id>`,
  for example `ANVIL_BENCHMARK=without:bellows`. The same variable marks the
  start as a benchmark start for the core and for plugins (`active()`). Any
  other value is ignored with a warning in `anvil.log`, and the start is then
  not a benchmark start.
- In the arm without, the core treats the compared plugin as disabled at the
  point of the resolver where `disabled_plugins` applies (state `disabled`), so
  both arms pay mod discovery. A plugin the player disabled stays disabled by
  the player. `anvil.toml` is never written, so nothing needs restoring if the
  Manager dies mid-series (anvil#131).

### The time

- A start's **time** is the milliseconds from the creation of the process to
  `anvil-game.ready`, taken from the start record (section 6).
  `Running application` is not used: it has no timestamp and comes 1.3–1.6 s
  before ready, so benchmark times are about 1.5 s longer than those of the
  Bellows README table (anvil#128, anvil#130).

### Ending

- In a benchmark start, `anvil-game` pushes `SDL_QUIT` on the main thread once
  `anvil-game.ready` has fired and every benchmark participant has called
  `done()`, or 30 s after ready. The core decides when: after its ready record
  it waits until every plugin is loaded and every participant is done (at most
  30 s after ready), writes the record a last time (`"write": "end"`) and publishes
  `anvil.benchmark_quit`; `anvil-game` then pushes `SDL_QUIT` through SDL's
  `SDL_PushEvent` after the next frame, once it has checked that SDL's own
  table still points there. This quits like the menu's Quit: exit code 0,
  about 1.1 s, `settings.txt` unchanged, no restart, safe mode untouched
  (anvil#134). `anvil-game` loads in both arms.
- **Fallbacks by the Manager:** 45 s after ready, if the process still runs, it
  posts `WM_CLOSE` to the game's window (SDL turns it into `SDL_QUIT`); 15 s
  later it kills the process. A killed start keeps its time (ready was reached)
  and its row is marked `killed`, but the series stops with `quit_failed`,
  since every later start would begin from a changed `settings.txt`.
- **Before ready** the game does not take `WM_CLOSE`. With no ready after
  600 s the Manager kills the process (`timeout`); that start counts towards
  safe mode.
- If the Manager exits or crashes mid-series, the running game still quits
  itself (the marker is in its environment); the series is lost without a
  report.

## 3. A benchmark series

### Preconditions

Before the series starts, the Manager checks:

- Steam is running (blocks the series; not asked in a GOG install, which starts
  without Steam and without `SteamAppId`).
- No `eu4.exe` is running.
- The safe-mode counter will not trigger at the next start.
- The compared plugin is enabled and approved, with no pending Workshop update.
- Running on battery gives a warning only.

It records locally the mod set (`dlc_load.json`) and the compared plugin's
version and SHA-256. Before and after every start it checks that Steam still
runs and that none of these changed; any change stops the series with
`environment_changed` (anvil#131), and a change found after a start also
discards that start.

### Order and count

- **ABBA pairs** (A = without the plugin, B = with it): A B B A A B …. The
  first arm alternates between series; the Manager remembers the last series'
  first arm locally, never in a report.
- **Between starts:** wait for the process to end, then a fixed 5 s pause.
- **Target:** valid warm starts per arm, 5 by default, with a choice of 3 / 5 /
  10. A start that does not count (discarded, unclear or cold) leaves its arm
  short; the Manager appends starts at the end, alternating between the arms
  that are short. The first appended start goes on with the ABBA order when
  both arms are short.
- **At most 4 extra starts per series.** Discarded and unclear starts use this
  budget; cold starts do not. Over the budget the series stops with
  `too_many_discarded`. So that a computer where every start is cold still
  ends, a fifth cold start stops the series with `too_many_discarded` too.
- **Stop by the player:** the running start finishes cleanly (ready, `done()`,
  quit), then the series ends as `stopped` with reason `player`.

### Which starts count

| `discard_reason` | When | Series |
| --- | --- | --- |
| `plugin_learning` | a participant called `learning()` | replaces the start |
| `crash` | the process crashed before ready | replaces the start; the second crash in a series stops it (`crashed`), as a third start in a row would be in safe mode |
| `safe_mode` | the core reports the compared plugin `skipped:safe_mode` | stops (`safe_mode`) |
| `arm_mismatch` | the compared plugin's state does not match the arm (for example `skipped:verification`, `failed`) | stops (`plugin_not_loaded`) |
| `not_participant` | in a start with the plugin, the compared plugin is `loaded` but is not a benchmark participant | stops (`plugin_not_participant`); the result says to update the plugin |
| `timeout` | no ready within 600 s; the Manager kills the process | stops (`timeout`) |
| `no_record` | the process exited without a start record and without a crash | replaces the start |
| `environment_changed` | Steam, the mod set, or the plugin's version or hash changed | stops |

- A crash after ready keeps the start's time (the row stays valid); the row
  carries the crash and its attribution level, and the series stops
  (`crashed`). So does a killed start, with `quit_failed`.
- A participant with `done: false` does not invalidate the time.
- `not_participant` (anvil#136) keeps v1 to plugins that join the benchmark: a
  plugin that does not call `done()` could have its after-ready work cut off
  by the quit, and one that cannot call `learning()` could have a learning
  start counted. For example, a local Bellows 0.9 on Anvil with the tab.
- Stop reasons: `player`, `too_many_discarded`, `quit_failed`, `crashed`,
  `safe_mode`, `plugin_not_loaded`, `plugin_not_participant`, `timeout`,
  `environment_changed`. The result has one sentence in words for each.

### Warm, cold and unclear starts

- The **open cost** decides: the time per file of the first opens of the
  game's existing files in that start. In the arm with the plugin, a
  participant's `open_cost(files, ms)` replaces it (for Bellows,
  `open_warmup`; the compared plugin's when several participants report
  one); otherwise Anvil measures the game's own first opens. The
  measurement is passive, since a probe would warm the start (anvil#129).
  A start without an open cost (no first opens counted) is unclear.
- Each signal has two thresholds with a band between them:

  | Signal | Warm | Cold |
  | --- | --- | --- |
  | the game's first opens (mean per file) | ≤ 0.2 ms | ≥ 1 ms |
  | a participant's `open_cost` (per file) | ≤ 0.04 ms | ≥ 0.2 ms |

  The thresholds are constants of the Manager, starting values from one lab
  machine (warm ~0.04 ms and cold ~4 ms per first open; `open_warmup` ~0.1 s
  warm and ~9.1 s cold for ~11.5 k files).
- **Unclear starts** fall in the band. They join neither group and use the
  extra-start budget.
- **Labels only describe a start:** *first start since boot* (boot time from
  `GetTickCount64`, kept by Anvil between starts) and *definitions changed*
  (Microsoft Defender event 2000, readable without elevation). They never
  override the cost: a start after a reboot with a warm cost is warm.
- **Cold starts** are not discarded. They form the cold group and do not count
  towards the warm target.
- The raw values stay in every row, so old reports can be reclassified when the
  thresholds change.

### Measurement conditions

- The Manager asks the player not to use the computer during the series.
- Every row gets a raw `input_during_start` flag (`GetLastInputInfo`, no
  elevation). It does not discard the start and is shown next to the result.

## 4. The result

The Manager computes the result from the rows; the same result goes into the
report, marked as derived (anvil#131).

- **Warm group, per arm:** median, min–max, n and every time.
- **Difference of the medians** in seconds and percent, with the plugin minus
  without it. When the min–max ranges overlap: "Ranges overlap: the difference
  is uncertain." No significance tests or confidence intervals: at n = 3–10
  they would be false precision.
- **Cold starts** one by one: arm, time and kind (after boot / after a
  definitions update / cold by open cost). A cold difference only when both
  arms have a cold start.
- **Starts that do not count:** how many, with their reasons; an unclear start
  shows its open cost per file.
- The starts with `input_during_start`, which still count.
- **A "not measured" line** built from the data: no cold start in one arm;
  fewer starts than the target; the series was stopped; and always that the
  time runs to `anvil-game.ready`, not to `Running application`.

## 5. `anvil.benchmark` v1

A public core service: its header
[`anvil_benchmark.h`](../../include/anvil/anvil_benchmark.h) and C++ wrapper
[`anvil_benchmark.hpp`](../../include/anvil/anvil_benchmark.hpp) ship in the
SDK. A plugin looks it up with `get_service`; on an Anvil without it the lookup
returns NULL and the plugin keeps working without fields. No manifest key is
involved, so an older Anvil still loads the plugin (anvil#135).

### Joining and declaring fields

- A plugin declares its fields inside `anvil_plugin_load`. Declaring, even zero
  fields, makes it a **benchmark participant**, which promises one `done()`
  call. The schema is fixed for the whole start; nothing is declared after
  load.
- Any loaded plugin may join, not only the compared one (`anvil-game` and
  `anvil-overlay` included). The player sees everything in the preview.
- **Kinds:** `int` (int64), `number` (double; NaN and infinity are refused),
  `enum` (an option name from the declared list) and `flag` (bool). An optional
  unit from a closed list: `ms`, `count`, `bytes`, `mb`, `percent`. There is no
  string kind; the kinds are the barrier that keeps paths and text out, against
  mistakes rather than malice.
- **Names:** `group.field` or `field`, from identifiers `[a-z0-9_]`, zero or one
  dot; the prefix groups fields in the preview (`open_warmup.opened`). Enum
  options follow the same rule.
- **Labels:** an optional short English label per field, shown in the preview
  only, never in the report.
- **Limits:** at most 128 fields per plugin, 32 options per enum, 64 characters
  per name and 80 per label. Going over a limit, an unknown field or the wrong
  kind gets an error code and one warning in the log; the plugin keeps running.
- **Meaning:** a name never changes its meaning or kind; a new meaning gets a
  new name. There is no per-plugin schema version: the report carries every
  plugin's id, version and SHA-256.

### Calls during a start

- `active()`: 1 in a benchmark start, 0 otherwise. Outside a benchmark start
  the calls check their arguments the same way and nothing is recorded, so a
  plugin may call it always or skip work when it is inactive.
- `set(field, value)` (in C `set_int`, `set_number`, `set_enum` and
  `set_flag`): from any thread; the last value wins. A field never set
  is `null` in the row. Values set after the plugin's `done()` are ignored.
- `done()`: the participant's after-ready work is finished (for Bellows: the
  profiles are saved and the module reports are made). The quit waits for every
  participant, at most 30 s after ready; a late participant's entry gets
  `done: false` with the values set so far.
- `learning(reason_enum)`: this start is not representative for the plugin
  (for Bellows: the first start on a new mod set, a profile still being built).
  The start is discarded with `plugin_learning` and replaced.
- `open_cost(files, ms)`: the participant opened the files ahead of the game
  (for Bellows: `open_warmup`). In the arm with the plugin this is the start's
  open cost.

## 6. The start record

- The core writes one record per benchmark start and the Manager reads it
  after the process exits. Each write is atomic. Its location and encoding are
  implementation details; the per-plugin files, `anvil.status` and the log are
  not used (the log rotates and contains paths). In Anvil 1.4 it is
  `anvil/data/anvil/benchmark-start.json`, one JSON object with `"format": 1`,
  replaced by every benchmark start; a benchmark start removes the previous one
  at its entry point, so a start that never reaches ready leaves none. The
  record carries the process id and creation time, which the Manager compares
  with the process it started.
- **Two writes:** at `anvil-game.ready` (the time, the plugin states and the
  fields set so far; written on a thread of the core, off the game's main
  thread) and a final one when the quit is decided (section 2), with every
  `done` and the values set by then (`"write": "ready"` or `"end"`). The final
  write is not at exit: in two lab starts that quit through `SDL_QUIT`, the
  game ended its process without Anvil's `DLL_PROCESS_DETACH` handler running
  (it wrote no log line), so a write there would never happen. The cause
  (anvil#164): the game's C runtime calls `ExitProcess`, and a DLL that
  detaches before Anvil (a graphics driver behind the Epic overlay's detach)
  ends the process there. The core now runs its exit work (the
  call counters, safe mode) from its replacement of that import instead, but
  the record keeps its final write at the quit decision. Values a late
  participant sets after the final write are lost; its entry has
  `done: false`. A crash after ready leaves the
  first write; a crash before ready leaves no record, so that row carries only
  the crash and its attribution level.
- **Contents:**
  - the timing: QPC at the entry point (`AnvilEntry`) and at ready, the precise
    system time at entry and the process creation time, from which the Manager
    computes the time to ready (the creation → entry gap is not logged
    otherwise; lab runs bound it at ≤ 0.1–0.3 s); the record also gives the
    three differences in ms (`creation_to_entry_ms`, `entry_to_ready_ms`,
    `time_ms`), so no 64-bit counter has to pass through a JSON number;
  - the arm the core saw;
  - every discovered plugin's state: `loaded` (with `done` and its fields),
    `disabled` (the arm's switch or the player), `skipped` with a reason enum
    (`safe_mode`, `conflict`, `dependency`, `verification`, `approval`,
    `other`; Anvil's reason text never appears) or `failed`;
  - each participant's field schema and labels, and any `learning()` reason;
  - the open cost: each participant's `open_cost`, and Anvil's measurement of
    the game's first opens until ready (`game_first_opens`: the successful
    `CreateFileW` calls of `eu4.exe` that read an existing file, the first time
    each path is opened, timed by the core's own import replacement, the
    innermost link of that chain, in benchmark starts only); the Manager picks
    the one section 3 names;
  - the cold-start labels (first start since boot, definitions changed) and
    `input_during_start` (`GetLastInputInfo` later than the entry point), each
    `null` when it cannot be known. Every start, a benchmark start or not,
    leaves its boot time, tick count and entry time in
    `anvil/data/anvil/last-start.txt`; "first start since boot" means the tick
    count went back or the boot time moved by more than two minutes since that
    start, and "definitions changed" means a Defender event 2000 is newer than
    that start's entry point.

## 7. The benchmark report

One JSON document per series. The local copy, the preview, what **Send**
uploads and what **Copy** copies are the same bytes (anvil#130). The exact
document is the JSON Schema of its format,
[`report-format-1.schema.json`](../../services/bench-receiver/schemas/report-format-1.schema.json)
for format 1, with an
[example](../../services/bench-receiver/examples/report-format-1.json).

### Contents

- **Envelope:** `format` (an integer, section 9), a random **series id** made
  new for every series (only to drop a repeated Send), and the series state:
  `complete`, or `stopped` with its reason. Stopped series can be sent; they
  are often the most useful signal.
- **One row per start, discarded starts included:**
  - arm and position in the order;
  - time to `anvil-game.ready` in ms;
  - valid or discarded, with `discard_reason`; `killed` when the Manager
    killed it;
  - the raw cold-start signals (first start since boot, definitions changed,
    the open cost with its source and file count) and the derived
    warm / cold / unclear label;
  - `input_during_start`;
  - a crash only as "crash", its attribution level (`in_plugin_code`,
    `in_plugin_change`, `plugin_on_stack`, `no_plugin`) and the plugin id;
  - every discovered plugin's state; for `loaded` plugins `done` and the field
    values under `plugins.<id>.fields`.
- **Plugin field schemas,** once per report (name, kind, unit, enum options),
  without labels.
- **Derived result** (section 4), marked as computed by the Manager.
- **System:** CPU model string, logical core count, RAM in whole GB, the drive
  kind (NVMe / SATA SSD / HDD / other / unknown) separately for the game
  folder, the EU4 user folder and the Workshop folder, the Windows build
  number, and the antivirus product names from Security Center.
- **Mod set:** number of enabled mods; the Workshop ids of Workshop mods (the
  player can leave them out); local mods as a count only; total file count and
  size of the enabled mods' folders, rounded.
- **Versions:** the game build identity (the known build's name from design §6,
  or the SHA-256 of an unknown `eu4.exe`), the Anvil version, and for every
  plugin loaded in each arm its id, version, source (local / Workshop + item
  id) and the SHA-256 of the loaded copy.

### Never in a report

Computer and account names, paths, drive models and serial numbers, GPU, free
disk space, names of local mods, the `dlc_load.json` hash, absolute dates and
times (rows have relative ms only), free text of any kind (no note field), log
lines (Bellows' `anvil.log` section prints paths), timing records, crash
reports, and any persistent install or player identifier.

### Kept on the player's computer

- The Manager keeps each report as `anvil/data/anvil/benchmarks/<time>.json`,
  the last 10, like crash reports.
- A sent report gets a marker `<time>.sent` beside it, since the JSON must stay
  byte for byte what was sent.
- The Workshop-id switch rewrites the kept file.

## 8. The Benchmark tab

A new **Benchmark** tab in Anvil Manager, between Settings and Crashes. Four
screens replace each other in that tab, one column, top to bottom (anvil#133,
prototype variant A). No entry point from the Plugins tab, and no in-game
notice: `anvil-overlay` does not change for the benchmark.

### 1. Setup (no series runs)

- One sentence on what happens: EU4 starts again and again, without the plugin
  and with it, each start quits by itself, and nothing in saves, mods or
  `anvil.toml` changes.
- **Compare:** a combo of the enabled, approved plugins.
- **Valid warm starts per arm:** 3 / 5 / 10, 5 by default, with the expected
  number of starts and minutes.
- **Before the series:** a ✓ / ✕ list of the preconditions (section 3) with the
  fix in words. Steam blocks; battery is a warning line.
- A request to leave the computer alone, saying that keyboard and mouse use is
  recorded next to each start.
- **Start the series** (disabled while a check fails) and **Check again**.
- Collapsed **Kept reports (n)**: finish time, compared plugin and version,
  series state, sent or not (from `.sent`), **Open** and **Open the folder**.

### 2. Running

- "Start k of about N" with the arm, an overall progress bar, and the phase:
  Starting eu4.exe / Loading (seconds so far) / Ready, waiting for plugins to
  finish (their ids) / Quitting / Pause before the next start.
- Valid warm starts per arm as two bars against the target, and "Extra starts
  used: x / 4".
- One chip per finished start: number, arm, time, and cold / unclear /
  discarded or the input mark.
- The request to leave the computer alone, and **Stop**, which then reads
  "Stopping: this start finishes and quits cleanly, then the series ends."
- While a series runs the game bar says a benchmark is running; **Play EU4**
  and **Change…** are disabled, and the other tabs are dimmed and locked until
  the series ends.

### 3. Result

- "Series complete", or "Series stopped: <reason in words>"; the number of
  starts, extra starts used and the target.
- **Warm starts table** (arm, n, median, min–max, every time) and a **dot
  strip**: one row per arm on a shared axis, a dot per warm start, a mark at
  the median, a line for min–max.
- **Difference:** for example "With Bellows: −7.4 s (−19.0 %)", coloured good or
  bad only when the ranges do not overlap; otherwise the overlap warning.
- **Cold starts** table and the cold difference, or a line that it needs a cold
  start in both arms.
- **Starts that do not count**, the `input_during_start` line, and the **not
  measured** line of section 4.
- **Review the report and send…**, **New series**, and the kept file's name.

### 4. Preview ("The report, exactly as it is sent"; **< Result** goes back)

- **Result** in one line, and the counts of starts, cold starts and starts that
  do not count.
- **Your computer:** processor and cores, memory, drive kind per folder,
  Windows build, antivirus names.
- **Mods:** counts, files and size, and the only switch, **Include the Workshop
  ids of my mods** (on by default), listing the ids while it is on.
- **Versions:** EU4 build, Anvil, every plugin with version, source and
  SHA-256.
- **Plugin state in each start:** a plugin × start table, plus "not finished"
  for `done: false`.
- **Fields reported by <plugin>:** one start at a time, grouped by prefix, with
  the label, value and unit; enums as option names; a warning for a start with
  `done: false`. Notes say that labels appear only here and list what is never
  in a report.
- **Show the JSON**, open by default: the exact bytes kept, sent and copied.
- **Send** and **Copy** with the file size; the outcome is one line under them
  (section 9). Send becomes a disabled **Sent** after 201 or 200.
- The privacy note (section 10) with this report's series id.

## 9. The benchmark receiver

Decided in anvil#132.

### Endpoint and code

- `POST https://bench.happydwarf.dev/reports`: a Cloudflare Worker on the
  Workers Free plan of the Happy Dwarf account, behind a custom domain. The
  URL is compiled into the Manager.
- The code lives in this repository, `services/bench-receiver/`: the Worker, a
  JSON Schema per report format, the D1 migrations and the saved queries. The
  schema is the contract between the Manager and the Worker, so one pull
  request changes both, and the Manager's tests validate their reports against
  it.
- Deployment is manual: a maintainer runs `wrangler deploy` with a token from an
  environment variable. CI tests the Worker and holds no Cloudflare secret.

### Storage

- One D1 database `anvil-bench`, one table `reports`: `series_id` (primary
  key), `format`, `received_day` (the **day of receipt**, a UTC date) and
  `body`, the raw text byte for byte. Envelope fields (Anvil version, compared
  plugin and version, series state) are indexed generated columns from
  `json_extract`; per-format views read rows with `json_each`. No R2.
- A second table, `daily_counts`, holds the number of reports and bytes received
  per UTC day for the daily limits, so a request reads one row instead of counting
  the day's reports; past days are deleted daily.
- Nothing else is stored: no IP address, country, User-Agent or time of day.
  The IP lives only in the rate limiter's counters.
- `invocation_logs = false`; one log line per request with status, format and
  size, never the IP, the body or the series id.
- Kept 24 months from the day of receipt; a daily Cron Trigger deletes older
  reports.

### Checks, in order

1. Body up to 256 KB (413), `application/json` only.
2. 5 requests per 60 s per IP through the `ratelimit` binding (429).
3. At most 2000 new reports per UTC day across all senders (503).
4. Validation against the format's schema (422; an unknown format says "update
   Anvil"), plugin fields checked generically against the schemas in the same
   report, plus plausibility checks (1–200 starts, times in a sane range,
   medians consistent with the rows).
5. `INSERT OR IGNORE`: a repeated `series_id` answers 200 "already received"
   without a write; a new one 201.

The numbers are starting values in the Worker's configuration. No secret, no
CAPTCHA: the Manager is open source, so a report cannot be proven genuine; the
data is a diagnostic signal, and bad reports are removed by hand.

| Response | The Manager shows |
| --- | --- |
| 201 | "Sent" |
| 200 | "This report has already arrived" |
| 413 / 422 | the server's message |
| 429 / 503 | "Try again later" |
| network error | the error, with Copy |

No automatic retries: the player clicks again.

### Format versioning

- `format` starts at 1, and format N is exactly schema N: any change to
  Anvil's schema bumps it, an added field included. Plugin fields under
  `plugins.<id>.fields` never bump it.
- The receiver keeps every accepted format's schema and gets a new one deployed
  before a Manager that sends it is released. Old formats stay accepted until
  one is retired explicitly.

### Reading reports

Maintainers read with `wrangler d1 execute --remote`, the D1 console and the
saved queries in `services/bench-receiver/`, and export with `wrangler d1
export` for offline analysis. Access is the Cloudflare account's members only;
there is no admin UI and no notification of new reports.

## 10. Privacy

- Under Send and Copy a short note says where the report goes
  (`bench.happydwarf.dev`, Cloudflare), that Cloudflare sees the IP for rate
  limiting and it is not stored, that the server adds only the day of receipt,
  that reports are kept 24 months, and how to ask for deletion: the report's
  series id, sent to `privacy@happydwarf.dev`.
- The full text is `docs/benchmark-privacy.md` in this repository, linked from
  the note.
- Deletion is by hand by a maintainer, by series id; there is no deletion API.
  Requests go to the e-mail address rather than a public issue, which would tie
  a GitHub account to a report.
- `privacy@happydwarf.dev` is a Cloudflare Email Routing address forwarding to
  the maintainer.

## 11. Releases

Decided in anvil#136. Anvil releases its core, Manager, `anvil-game`,
`anvil-overlay` and SDK together under one version, so the benchmark ships in
two Anvil releases with a Bellows release between them. The versions below
assume no other minor release comes first.

### Anvil 1.4: the game side

Nothing a player sees. It contains:

- `anvil.benchmark` v1 in the core, with its header and C++ wrapper in the SDK
  (section 5);
- the arm in the start's environment and the compared plugin disabled at the
  `disabled_plugins` point (section 2);
- the start record, the game's first-open cost, the cold-start labels and
  `input_during_start` (section 6);
- `anvil-game`'s `SDL_QUIT` after ready and every `done()` (section 2).

It is tested with unit tests and a lab start that sets the environment by hand.

### Bellows 0.10: a benchmark participant

- First, in its own pull request, `SB_ANVIL_VERSION` moves to Anvil 1.4, so the
  build has the service's header.
- Then one `feat` release: Bellows declares its fields (per module the mode as
  an enum `off` / `on` / `verify` / `failed`, its counters as `int`, finish
  times in `ms`, the profile source of `open_warmup` and `ram_prefetch` as an
  enum), calls `learning()` on its first start on a new mod set, `open_cost()`
  for `open_warmup`, and `done()` at the end of its ready-report thread. Its
  `anvil.log` lines stay as they are.
- The manifest keeps `anvil = ">=1.0.0"`: on an older Anvil `get_service`
  returns NULL and Bellows runs without the benchmark.
- Published to the Steam Workshop as usual, so Workshop players have it before
  the tab exists.

### Anvil 1.5: the Benchmark tab

- The Manager's series, result, report format 1, the Benchmark tab and Send /
  Copy (sections 3, 4, 7, 8).
- `services/bench-receiver/` with the schema of format 1 (section 9) and
  `docs/benchmark-privacy.md` (section 10). Both may reach `main` earlier; they
  are not part of what release-please ships.
- `CONTRIBUTING.md` gets a release checklist for a release that changes the
  report format.

**Gate:** the release pull request of Anvil 1.5 is merged only when all four
are true:

1. The receiver with the schema of format 1 is deployed to
   `bench.happydwarf.dev` and has accepted a test report.
2. `privacy@happydwarf.dev` exists and forwards mail.
3. `docs/benchmark-privacy.md` is on `main`.
4. A maintainer has run one Bellows series on the lab machine with a build of
   `main` (following the lab's procedures) through the whole chain: series,
   result, preview, Send, a row in D1, the saved queries; the test report is
   then deleted by hand.

### After Anvil 1.5

Documentation only, in Bellows (`docs:`, no release of its own):

- `performance_report.yml` gets an optional "Benchmark report (JSON)" field;
  the hand-typed fields stay, as optional.
- The README points to the Benchmark tab next to its measured table, with the
  note that benchmark times run to `anvil-game.ready` and are about 1.5 s
  longer.

### Blocking, in short

- Anvil 1.4 blocks the Bellows SDK bump, which blocks Bellows 0.10.
- Bellows 0.10 and the four conditions of the gate block merging the release
  pull request of Anvil 1.5.
- Anvil 1.5 blocks the Bellows issue template field and README line.
- Nothing blocks Anvil 1.4; a player on Anvil 1.4 with Bellows 0.10 sees no
  difference.

## 12. Out of scope for v1

- A public page of aggregated results.
- A vanilla start without Anvil as a third arm.
- Cold starts made with administrator rights (UAC, flushing the system cache),
  and per-file cache eviction presented as a cold start: it slows reads but not
  the first opens that make a reboot's start slow (anvil#129).
- A multi-reboot "cold series" (one start per reboot, arms alternating).
- An in-game notice during a benchmark start.
- A note field, a deletion API, an admin UI, notifications of new reports.

## 13. Open points to check during implementation

These are hypotheses or untested paths, not decisions:

- Whether Steam needs the 5 s pause to register the game's exit.
- The warm and cold thresholds, taken from one lab machine. Anvil 1.4's own
  measurement of the game's first opens (11 817 files until ready) gave
  0.031 ms per file without Bellows and 0.034 ms with Bellows 0.9 loaded but
  not a participant, and 0.33 ms (inside the band) in a start with Bellows
  that came first after a pause and reached ready about 4.8 s later than a later
  one; n = 1 each. With a plugin that opens the game's files ahead of it, the
  game's own first opens are not a clean signal, which is why its
  `open_cost` replaces them.
- The boot marker under Fast Startup, which restores the kernel from
  `hiberfil.sys` and may not reset `GetTickCount64`; untested.
- The Manager's own `WM_CLOSE` fallback: tried from a script, not from the
  Manager (again with Anvil 1.4 on a start without the arm: exit code 0,
  `settings.txt` unchanged).
- Quitting while a dialog is open at ready; not tried.
- `input_during_start` was true in both lab starts with Bellows and false in
  the one without (n = 1 each). Bellows sends no input, so it was most likely
  real use of the computer during those starts, but that was not checked.
- Whether the cause of the reboot's ~4.45 ms first-open cost is Defender's
  filter or cold NTFS metadata (needs an elevated trace; it does not change
  the classification).
