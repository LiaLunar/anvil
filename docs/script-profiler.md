# The script profiler

A window of Anvil's overlay for modders that times the game's triggers and effects by the event,
decision, on_action, casus belli, triggered modifier or mission they belong to, and writes the
results to a CSV file. It is the standard plugin `anvil-script-profiler` (design §4), loaded with
Anvil and idle until a modder starts a measurement: it hooks nothing and starts no thread before
that. It only times, so `affects_gameplay = false`, and it works in ironman and multiplayer.

Its shape was decided in [eu4-engine-lab#158](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/158),
after the prototype of [#154](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/154); the
work was [#163](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/163). Terms are in the
glossary: script block, root block, owner, profiling span.

## Measuring

- The window is **Script profiler** in the **Status** tab of the Anvil panel, hidden until the modder
  shows it. Its buttons appear while a game is loaded:
  - **Start** starts a profiling span, **Stop** ends it;
  - **Measure N days** starts a span that ends by itself after N game days (the setting **Days to
    measure**, `days`, 30 by default).
- During the span the window shows the days and seconds so far. The game runs slower: see "Cost".
- A load, leaving to the menu or quitting the game ends the span. What was measured until then is
  kept and written, marked interrupted (the window says so). On a quit the file is written only when
  the game exits normally (the menu's Exit); a crash writes nothing.
- In multiplayer the window warns that the measurement slows that player's game, and every player
  waits for the slowest.
- There is no console command and no automatic start.

## What it shows

After a span the window shows how long it was (days, dates, seconds), where the CSV was saved, and two
lists, **Triggers** and **Effects**, each with its total time and calls and then its longest rows
(the setting **Rows per list**, `rows`, 20 by default). A row is an owner with a role:

| Owner | Roles |
| --- | --- |
| Event | `trigger`, `major_trigger`, `mean_time_to_happen`, `immediate`, `after` |
| Event option, shown as the event's key and the option's number (`#2`) | `trigger`, `effect`, `ai_chance` |
| Decision | `potential`, `allow`, `effect`, `ai_will_do`, `provinces_to_highlight` |
| On action | `effect` |
| Casus belli | `prerequisites`, `prerequisites_self` |
| Triggered modifier (a country's) | `potential`, `trigger` |
| Province triggered modifier | `potential`, `trigger`, `on_activation`, `on_deactivation` |
| Mission | `trigger`, `effect`, `ai_weight`, `provinces_to_highlight` |
| Church aspect (from 1.22's v1.1) | `trigger`, `potential`, `effect`, `ai_will_do` |
| Scripted trigger, when one runs as a root block | `trigger` |

Every block of one owner and one role is one row: an event's `mean_time_to_happen` modifiers make one
row, as do all blocks of one decision's `ai_will_do`. Each row has the total time, the calls, the mean
and the longest single run (`max`), which shows a rare effect that takes tens of milliseconds at once.
Blocks whose owner the profiler cannot tell (an `OR` or `NOT` run on its own, a `limit`, the AI's own
checks) are one row **No owner found** per list.

A third list, **Scripted triggers**, has every call of a scripted trigger by its name: the outermost
one on its thread is timed wherever it runs, also inside an event's or a decision's trigger, so its
time is part of a row of the trigger list too. All uses of one scripted trigger are one row.

Times are summed over every game thread. A trigger root inside an effect (a `limit`) counts in both
lists: the effect's time includes it.

## The CSV file

`logs/script_profile_<date>_<time>.csv` in the game's user folder (the `-userdir=` folder, else
`Documents/Paradox Interactive/Europa Universalis IV`), one per span, never overwritten:

- line 1, `#` and `key=value` pairs: `format=1`, `plugin`, `version` (Anvil's), `build` (the game build),
  `status` (`complete`, `interrupted_session` or `interrupted_exit`), `started` (local time),
  `first_day`, `last_day`, `days`, `wall_s`, `threads`, `root_calls`, `nested_triggers`,
  `nested_effects`, `dropped_calls` (see "Limits"), `multiplayer`, `ironman`;
- line 2, `#` and a note on the columns;
- then a header and one row per owner and role, triggers first, each list longest first:
  `list,rank,owner,name,option,role,calls,total_ms,mean_us,max_us,main_thread_ms,caller_rva`.
  `owner` is `event`, `event_option`, `decision`, `on_action`, `casus_belli`, `triggered_modifier`,
  `province_triggered_modifier`, `mission`, `church_aspect`, `scripted_trigger` or `none`; `main_thread_ms` is the part of the time on the
  game's main thread. Rows without an owner are split by `caller_rva`, where in `eu4.exe` the block was
  run from (an address relative to the image, to look up in the game's code).

## How it works

- **Hooks, only during a span.** Three detours, put in together when a span starts and removed when it
  ends: `CAndTrigger::Evaluate` (Steam `0xB98200`), which every `AND` block runs through, the root of each
  trigger and each limit; and `CEffect::Execute` (Steam `0x5ED800`), which every block of effects runs
  through; and `CScriptedTrigger::Evaluate` (Steam `0xB99B30`), every use of a scripted trigger. `anvil.log` says when the span starts ("profiling span started: hooks on …") and ends ("hooks
  removed"); the plugin's load line says `0 change(s)`.
- **Recording.** Each game thread has a table of its own (65 536 slots, made on its first block and kept
  for the process), written only by that thread, without a lock and without sampling. On each thread only
  the outermost block of each kind, the root, is timed (`rdtsc`, turned into seconds against
  `QueryPerformanceCounter` over the span); blocks inside it are only counted. At the end of a span the
  tables are summed.
- **Owners.** A root block is a member of its owner at a known offset, and each owner class is told by its
  vtable (`symbols.toml`, checked by `symbols.lock`), since `eu4.exe` has no RTTI for its own classes. Blocks
  an owner holds elsewhere (an event's options and `mean_time_to_happen`, a decision's `ai_will_do`, a
  mission's `ai_weight`) are found from the owner once one of its member blocks was seen in the span. The
  owners are looked up on the main thread at each day's end and at the end of the span; every read is
  guarded, and a block that is not what the tables expect has no owner. The owner's name is its key
  (`_DebugKey` for an event, `_Tag` for the rest).
- **Church aspects** (v1.1): `CChurch::FindCheapestAspect` evaluates the children of each aspect's
  `trigger` and `potential` one by one (its own loop), so those children are the root blocks. They are
  found from the game's aspect database (`CChurchAspectDatabase::_pDB`, read through a code site in
  `symbols.toml`) when the first day of a span ends.
- **Scripted triggers** (v1.1): a use of a scripted trigger in a script is a `CScriptedTrigger` object,
  an `AND` block itself; its name is its template's key.
- **Builds.** Steam 1.37.5, GOG 1.37.5.2 and GOG 1.37.5.0. A build that declared an owner's vtable absent
  would still measure, without that owner's names.

## Cost

- Without a span: no hook, no thread, no work in a frame or a day; the panel is a row in the Status tab.
- During a span (eu4-engine-lab#164, lab series on Steam 1.37.5, process on CPU 1–7, 6 workers in each pool,
  ABBA rounds of 180 days at speed 5): vanilla, the 1719 Russia save reloaded before each segment, 9.80 days
  per second measuring against 9.98 idle, **−1.7%** (rounds −4.5% to +1.3%); Anbennar 1445–1450 in one game,
  **−1.0%** (rounds −7.0% to +5.8%), with 54 M root calls in 181 days (vanilla 8.3 M). Both are within the
  spread of the runs: the cost is smaller than this series resolves, below the 5% gate of #158.
- With v1.1's third hook (eu4-engine-lab#165, the same conditions): vanilla reloaded **+0.7%** (rounds −0.1% to
  +1.3%), Anbennar in one game **−0.1%** (rounds −6.3% to +5.1%).
- Memory: about 2.8 MB per game thread that ran a script block, kept after the first span.

## Coverage

The share of the trigger list's time that has an owner, from one span of 181 days (Steam 1.37.5, lab):

| Playset | v1 | v1.1 (scripted triggers, church aspects) |
| --- | --- | --- |
| Vanilla, Russia 1719 | 71.5% | 76.6% |
| Anbennar, Verne 1444 | 39.4% | 70.0% (84.7% of the calls) |

Most of what is left: the AI's decision checks (`CEU3AI::ExecuteDecisions`), `OR` blocks run on their own,
`limit`s (`CIfTrigger`) and, on Anbennar, `CReligiousSchool::CanInviteScholar`.

## Limits

- `OR` and `NOT` blocks that run on their own (not inside an `AND`), `limit`s and the AI's decision
  checks have no owner (eu4-engine-lab#165 left them without hooks).
- A thread's table holds 49 152 different root blocks; more in one span are summed into
  `dropped_calls` without a row.
- For very short blocks (well under a microsecond) the two `rdtsc` reads are a noticeable part of the
  time they measure.
- The window's texts are drafts in every language but English, not yet checked by native speakers.

## For the lab: the control file

For repeatable measurements without the window (the lab's overhead series), the environment variable
`ANVIL_SCRIPT_PROFILER_CONTROL` names a file. The plugin then reads it every 200 ms (a thread that runs
only with the variable set); a new line `<n> start`, `<n> measure <days>` or `<n> stop` (a new `<n>` each
time) is run on the main thread like the buttons, and the answer goes to `<file>.state`:
`<n> measuring` or `<n> idle <csv>`.
