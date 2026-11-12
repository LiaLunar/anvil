# Anvil

[![build](https://github.com/happy-dwarf-team/anvil/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/happy-dwarf-team/anvil/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/happy-dwarf-team/anvil)](https://github.com/happy-dwarf-team/anvil/releases/latest)
[![License: MIT](https://img.shields.io/github/license/happy-dwarf-team/anvil)](LICENSE)

Anvil is a plugin loader for **Europa Universalis IV** (Windows; Steam and GOG). It lets
native plugins improve or extend the game **without modifying `eu4.exe`**.

**Install guide, plugin catalog and docs for plugin authors:
[anvil.happydwarf.dev](https://anvil.happydwarf.dev).**

> Status: Anvil 1.x with plugin API 2 — see
> [Releases](https://github.com/happy-dwarf-team/anvil/releases) and
> [CHANGELOG.md](CHANGELOG.md). Supported game builds: EU4 1.37.5 (Steam,
> Windows 64-bit), from Anvil 1.7 the GOG build 1.37.5.2 and, from the next release, the
> GOG build 1.37.5.0; a plugin that uses game addresses loads only on the builds its
> `symbols.toml` lists (`anvil-game` lists all three), and on other builds only plugins that use no game addresses load. Design:
> [docs/design.md](docs/design.md). Plugin API 2:
> [include/anvil/anvil_plugin.h](include/anvil/anvil_plugin.h) (C) and
> [include/anvil/anvil.hpp](include/anvil/anvil.hpp) (C++20 wrapper); examples:
> [examples/hello-c](examples/hello-c) and [examples/hello-cpp](examples/hello-cpp).

## How it works (in short)

- Anvil ships as a `version.dll` placed next to `eu4.exe`. Windows loads it
  together with the game; it forwards every `version.dll` call to the system
  copy and, in addition, loads plugins.
- Plugins live in `anvil/plugins/<plugin-id>/` (manifest + DLL). They can also
  come in EU4 mods, from the Steam Workshop or the local mod folder, as
  `anvil/<plugin-id>/` in the mod. Such a plugin loads only while its mod is
  enabled in the playset and only after the player approves its exact files
  in the overlay or in AnvilManager.exe. An update waits for a new approval.
  Approvals are kept in `anvil/trust.toml`, and an approved plugin loads from
  Anvil's own copy in `anvil/data/anvil/approved/`.
- A plugin that uses game addresses declares them, for one EU4 build, in its
  `symbols.toml`; on another build it is skipped. Broken plugins, plugins
  written for an older plugin API or an empty `plugins` folder mean the game
  simply starts as usual. The manifest format is described in
  [docs/design.md](docs/design.md#51-plugintoml-manifest-v2).
- Anvil ships the standard plugin `anvil-game` (`anvil/plugins/anvil-game/`):
  game readiness, a frame event, work on the main thread, game sessions and
  the game date ([include/anvil/anvil_game.h](include/anvil/anvil_game.h)), reads of the
  running campaign (`anvil-game.world`) and, from Anvil 1.17, the unit panel's
  commands and an event's options for gameplay plugins (`anvil-game.commands`).
  Plugins that change the simulation need it: in ironman and multiplayer games
  Anvil takes their changes out unless the player allows them in
  `anvil/anvil.toml`.
- Anvil also ships `anvil-overlay` (`anvil/plugins/anvil-overlay/`): in-game
  notices about plugins that were not loaded, the previous crash and safe
  mode, and a panel on `Ctrl+Shift+F12` with the status, "disable at the next
  start", the crash report, the log and every plugin's settings. In the game's
  main menu a small "Anvil x.y.z" badge on the left edge shows that
  Anvil runs; a click on it opens the panel (the overlay's setting "Show Anvil
  in the main menu" turns it off). From Anvil 1.14, if you agree (it asks once, or follows your
  answer in the Manager), it asks GitHub for the latest Anvil release when the
  game is ready and tells you when a newer one is out, with a link; nothing is
  downloaded ([ADR 0005](docs/adr/0005-release-check-in-the-game.md)). From
  Anvil 1.18 it also gives plugins keys and mouse buttons of their own
  (`anvil-overlay.input`, [include/anvil/anvil_input.h](include/anvil/anvil_input.h)). It
  draws on the game's Direct3D 9 renderer and stays off with OpenGL.
- The overlay and the Manager speak the language EU4 is set to: English,
  French, German or Spanish (other languages show English). The Manager
  speaks Simplified or Traditional Chinese, Japanese or Korean when Steam or
  Windows is set to it, from its first start; the overlay does so with
  [Mei](https://github.com/happy-dwarf-team/mei), which shows a translation
  mod's Chinese, Japanese or Korean. They speak Polish when Steam is set to
  Polish. The overlay's setting "Language" chooses one for both. `anvil.log`
  and `anvil check` stay in English, for bug reports and plugin authors.
- And `anvil-log-throttle` (`anvil/plugins/anvil-log-throttle/`): a line that
  repeats in the game's `error.log` is written 10 times, then only counted,
  with a line that gives the count every minute and when the game closes the
  log. A mod whose script errors repeat hundreds of times a second no longer
  fills the log with hundreds of megabytes. Its settings (the overlay's panel)
  change the number of lines, the report interval and the logs, or write every
  line. From Anvil 1.20 its `panel` setting (off by default) opens a window of
  the game's script errors for modders, grouped by kind with their counts
  ([docs/script-errors-panel.md](docs/script-errors-panel.md)), and plugins read
  the same table through `anvil-log-throttle.messages`
  ([include/anvil/anvil_log_throttle.h](include/anvil/anvil_log_throttle.h)).
- And `anvil-map` (`anvil/plugins/anvil-map/`, from Anvil 1.16): plugins can add
  map modes of their own, a button on the minimap's list with a colour for each
  province and lines in its tooltip
  ([include/anvil/anvil_map.h](include/anvil/anvil_map.h)). By itself it changes
  nothing.
- And `anvil-script` (`anvil/plugins/anvil-script/`): a gameplay plugin can add
  words of its own to the game's script language, which a mod then writes in
  its events and decisions like any other effect, and have its work done once
  a day right after the game's day (the Day end). By itself it changes
  nothing.
- And `anvil-inspector` (`anvil/plugins/anvil-inspector/`, from Anvil 1.21): for
  modders, its `window` setting (off by default) opens a window with the flags,
  variables and timed modifiers of the province whose view is open, of its
  owner and of your country, with their rulers, heirs and consorts, as the
  running game holds them ([docs/inspector.md](docs/inspector.md)). It only
  reads.
- Each start writes `anvil/anvil.log`: what was loaded, what was skipped and why.
- A crash writes a report to `anvil/crashes/` (the last 10 are kept): where it
  happened and which plugins were involved, with each plugin's `issues` link.
  After two starts in a row that ended before the game was ready, the next
  start loads only `anvil-game` and `anvil-overlay` (safe mode), and the
  overlay says why.

## Install / uninstall

Download from the
[Releases page](https://github.com/happy-dwarf-team/anvil/releases) (the newest
build is always at
[releases/latest](https://github.com/happy-dwarf-team/anvil/releases/latest)).
`SHA256SUMS.txt` there holds the checksums of the assets.

Releases are not code-signed. To check a download, compare its SHA-256
(`Get-FileHash <file>` in PowerShell) with `SHA256SUMS.txt`, or verify the
build attestation with the GitHub CLI:
`gh attestation verify <file> --repo happy-dwarf-team/anvil`. An antivirus may
flag `version.dll` because it is a proxy DLL; such false positives can be
reported to Microsoft through the
[WDSI submission](https://www.microsoft.com/wdsi/filesubmission) page.
[Is it safe?](https://anvil.happydwarf.dev/docs/safety/) has the steps, and what
Anvil and a plugin can do on your computer.

**With Anvil Manager (`AnvilManager.exe`).** Run it, and it finds the game
through Steam or GOG (or lets you pick the folder that contains `eu4.exe`).
"Install Anvil" copies `version.dll`, the standard plugins and the Manager
into that folder; from then on, start the Manager there. "Add to the Start
menu" on its Anvil tab makes that one click, and uninstalling removes the entry.
- It shows which plugins will load and why, and turns plugins on and off.
- Its Catalog tab lists the team's plugins (Bellows, Tongs, Hamweard, Mei) and
  installs or updates one with a click (Anvil 1.9 and later): the Manager
  downloads the zip of the release the list pins, installs it only if its
  SHA-256 matches, and shows what `anvil check` finds first. From Anvil 1.15,
  if you agreed to the release check, it reads the list when it starts and shows which
  installed plugins have an update; no zip is downloaded until you
  click. See
  [ADR 0002](docs/adr/0002-manager-plugin-catalog.md) and
  [ADR 0006](docs/adr/0006-catalog-check-at-start.md).
- It installs a plugin from its release zip after checking it against your
  `eu4.exe`, and edits plugin settings.
- On its Mods tab it approves and revokes plugins that come with enabled mods
  (Anvil 1.3 and later), checking that the mod still has the version shown.
- On its Benchmark tab (Anvil 1.5 and later) it measures how long the game
  takes to start without a plugin and with it, in a series of starts that quit
  by themselves, and shows the result with cold starts apart. The report is
  sent to the maintainers only when you click "Send" after seeing it, or
  copied; see [docs/benchmark-privacy.md](docs/benchmark-privacy.md) and the
  [specification](docs/spec/benchmark-v1.md).
- It shows crash reports and the log, and starts the game through Steam or,
  in a GOG install, through the Paradox launcher, as GOG does. On
  its Log tab, "Copy a problem report" puts what an issue needs on the
  clipboard: the versions, the game build, the plugins and why one is
  skipped, the newest crash report and the end of `anvil.log`, and from
  Anvil 1.14 how many game folders Steam and GOG list, which one the Manager
  uses, an `eu4.exe` running from another folder and a last game without
  Anvil. The game folder, your user folder and Windows user name are left
  out. Nothing is uploaded.
- "Repair" puts back Anvil's own files.
- It recognizes EU4DLL and EU4 Unicode Patch, which Anvil replaces (Mei
  shows Chinese, Japanese and Korean text instead): "Replace EU4DLL" moves
  their files to `anvil-legacy-backup/` in the game folder and installs
  Anvil. Another mod's `version.dll` is replaced only if you tick the box,
  and also goes to that folder rather than being overwritten.
- "Uninstall Anvil" removes `version.dll`, the standard plugins and the
  Manager, and keeps your plugins, their settings and `anvil/anvil.toml`.
  Tick "Also remove my plugins…" to remove the `anvil` folder too, and
  "Restore … from the backup" to put back what Anvil replaced.
- To update Anvil, run the Manager of the newer release.
- While the game runs, the Manager only shows. From Anvil 1.14 it also says
  when the game runs from another folder than the one Anvil is installed in
  ("Switch to that folder" moves the Manager there), and when a game from its
  folder ran without loading Anvil; both point to
  [If Anvil does not run at all](https://anvil.happydwarf.dev/docs/troubleshooting/#if-anvil-does-not-run-at-all).
  It is not code-signed, so
  Windows SmartScreen may ask once ("More info" → "Run anyway").

**By hand.** Extract `anvil-windows-x64.zip` into the game folder (the
archive has its own `SHA256SUMS.txt` for the extracted files). To uninstall,
delete `version.dll`, `AnvilManager.exe` and the `anvil` folder.

`eu4.exe` is never changed, so Steam updates and "Verify integrity of game
files" keep working. Without `version.dll` the game starts vanilla.

## Help and bug reports

- [Troubleshooting](https://anvil.happydwarf.dev/docs/troubleshooting/) covers
  the log, skipped plugins, crash reports and safe mode. No badge and no
  `anvil/anvil.log` after a start means Windows did not load Anvil's
  `version.dll`: see
  [If Anvil does not run at all](https://anvil.happydwarf.dev/docs/troubleshooting/#if-anvil-does-not-run-at-all).
- Questions go to the `#help-and-bugs` forum on the team's
  [Discord server](https://discord.happydwarf.dev/) (English; tag the project).
- Bugs go to an [issue](https://github.com/happy-dwarf-team/anvil/issues/new/choose):
  **Copy a problem report** on the Manager's **Log** tab fills in what the form
  asks for. A problem in one plugin belongs to that plugin's tracker, the
  `issues` link in its manifest. What the team confirms on Discord ends up in
  an issue too.
- Security problems: report them privately, as [SECURITY.md](SECURITY.md) says.

## Plugins

The [plugin catalog](https://anvil.happydwarf.dev/plugins/) lists released
plugins with their install guides:

- [Bellows](https://github.com/happy-dwarf-team/bellows): faster start-up and
  simulation, without changing game files or rules; also on the
  [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3823747170).
- [Tongs](https://github.com/happy-dwarf-team/tongs): lets scripts and agents
  control a running game through a local pipe, with an MCP server; also on the
  [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3826695093).
- [Hamweard](https://github.com/happy-dwarf-team/hamweard): back from a campaign
  to the main menu without restarting the game; also on the
  [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3832650792).
- [Mei](https://github.com/happy-dwarf-team/mei): Chinese, Japanese and Korean
  text for translation mods; also on the
  [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3832685486),
  which carries its fonts.

The Manager's Catalog tab installs them from
[`catalog/plugins.json`](catalog/plugins.json).

[Etch](https://etch.happydwarf.dev/), a preview for EU4 interface mods in
Visual Studio Code, is built on Anvil too: its game host runs a private copy
of the game with pinned builds of Anvil, `anvil-game`, Bellows and Etch's own
plugin. It is not a catalog plugin. To write a plugin, start from the
[plugin template](https://github.com/happy-dwarf-team/anvil-plugin-template)
and the [author docs](https://anvil.happydwarf.dev/docs/plugins/overview/).

## Building

Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload, CMake
≥ 3.25, Git (dependencies are fetched at configure time: toml++ 3.4.0,
SafetyHook 0.7.0 with Zydis 4.1.0).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --component anvil --prefix dist   # version.dll + anvil/ layout
```

The build also produces `anvil.exe`, the command-line tool for plugin authors.
It shares the `anvil_core` library (manifests, `symbols.toml` and its
verification, `settings.toml`, the game build identity) with `version.dll`:

```powershell
anvil gen symbols.toml build\generated            # symbols.gen.h and symbols.gen.hpp
anvil gen settings.toml build\generated           # settings.gen.h and settings.gen.hpp
anvil gen --lang rust symbols.toml out            # symbols.gen.rs (Rust plugins)
anvil check my-plugin --exe "C:\...\eu4.exe"     # verify against the game, write symbols.lock
anvil check my-plugin --format github             # in CI, without the game: schemas and symbols.lock
anvil port symbols.toml --from old\eu4.exe --to new\eu4.exe --build <new id> --out symbols.toml
                                                  # a game update: place the symbols in the new build
                                                  # (--unknown-build for a build this anvil.exe does not know yet)
```

The Rust side (`sdk/rust/anvil-sys`, the safe crate `sdk/rust/anvil-sdk`,
`examples/hello-rust` and `examples/panel-rust`, see
[§9](docs/design.md#9-plugin-api-2)) is a Cargo workspace at the root; it needs
Rust 1.82 or newer and the `anvil.exe` built above, which the hello's `build.rs`
runs. CI uses Rust 1.99.1, and `sdk/rust/regen-bindings.ps1` (bindgen 0.73.2 and
libclang) regenerates the committed bindings after a change to a header:

```powershell
$env:ANVIL = "$PWD\build\Release\anvil.exe"
cargo build --workspace
cargo test --workspace
```

See [docs/design.md §16](docs/design.md#16-tooling-for-authors) for the
checks, the instruction-boundary rule and the `symbols.lock` format.

Contributions, the commit convention and the release process: see
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE). Anvil is an unofficial community project, not affiliated
with or endorsed by Paradox Interactive.
