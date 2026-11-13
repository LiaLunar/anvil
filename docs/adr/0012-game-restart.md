# Restarting the game: the game's own restart, asked through `anvil-game`

Status: **accepted** (2026-11-13).

A player asked for a way to restart the game from inside it (anvil#328). Without Hamweard, **Back** on the nation selection and **Exit to Menu** restart the game, and players use them as a quick restart; Hamweard turns both into a return to the main menu in the same process. A restart is still needed before multiplayer after a campaign, after a setting with `restart = true`, when a mod author changes a mod's files, and to get back the memory the game's reset keeps. This ADR decides how a restart is made, where it lives and what it does to a campaign that is running. Two choices are hard to undo once plugins use them: the restart is the game's own, not a new process Anvil starts, and it is a function of the `anvil-game.game` service that any plugin can call.

How to read the evidence. **[pdb]**: static, from GOG 1.37.5.2's `eu4.pdb` and the bytes of the three builds (lab `docs/anvil-menu-patch.md`, `docs/hamweard-ironman-default.md`). **[code]**: read in this repository or Hamweard. **[live]**: seen in the game, in this feature's pull request. **[hypothesis]**: not checked.

## Decision

1. **The restart is the game's own.** Anvil never starts `eu4.exe` itself. It sets the flags the game's own restart sets, and the game quits as from its menu and starts again through its own code.
2. **`anvil-game.game` gets `restart(flags)`** (anvil-game 1.23.0, appended to the service, `ANVIL_GAME_HAS(game, restart)`). It may be called from any thread and only queues the request; anvil-game acts on it after the next outer frame, on the main thread, with the state of that frame:
   - **No session** (the main menu, the nation selection, the other front-end screens): it writes the exit and restart flags of the application, `app+0x83` and `+0x84`, as one word `0x0101`, the store of the nation selection's **Back** (`CGameSetup::Back`).
   - **A single-player session**: only with `ANVIL_GAME_RESTART_CLOSE_CAMPAIGN` in `flags`, the caller's promise that the player accepted closing the campaign. It sets the in-game idler's `_bResignGame` (Steam `+0x12E9`, GOG `+0x12D9`), the flag the game's own Exit to Menu sets once the player confirms. The game then runs its own Exit to Menu: `NiceMultiplayerExit`, `HandleIronmanSave` and `CApplication::Restart`. With Hamweard the same branch is a return to the main menu; anvil-game keeps the request and writes the application's flags at the first frame of the main menu after the session ended, so the restart still comes, a return later.
   - **A multiplayer session, or a campaign without the flag**: refused, `ANVIL_ERROR_WRONG_PHASE`, both at the call (as of the last frame) and when anvil-game acts on it.
3. **The overlay's Status tab gets a "Restart game" button** that asks for a confirmation in the panel and then calls `restart`. The confirmation says what happens to a running campaign: an ironman one is saved as at Exit to Menu; any other loses its progress since the last save. In a multiplayer session the button is disabled, with the reason.
4. **Hamweard does not depend on this.** Its Shift+**Back** and Shift+**Exit to Menu** (hamweard#76) take the stock branch of the two blocks it already replaces, which is the game's own restart at the place the game makes it.

## What the game's restart keeps

[pdb] `CApplication::Restart` stores the two flags and copies its arguments to `app+0x88`; nation selection **Back** stores the flags alone with one `mov word [app+0x83], 0x0101`. `CApplication::Run` leaves its loop when `app+0x83` is set (the check anvil-game already declares as `main_loop_frame`), and after the loop, with `app+0x84` set, calls `CApplication::DoRestart`. That destroys the SDL window, takes `GetCommandLineA`, passes it to `PrepareCommandLineForRestart` and starts `GetModuleFileNameA`'s path with `CreateProcessA`; a failure is logged as `Failed to restart process`. `PrepareCommandLineForRestart` parses the line, removes `continue` (so the new process opens the main menu, not the last save) and appends the restart's arguments as `--name` or `--name="value"`; in the Steam build it also adds the Paradox account's sign-in (`AUTH_TYPE`, `refreshtoken`, `AUTH_PASSWORD`), in the GOG build those strings do not exist.

So the new process is the same `eu4.exe` with the same launch options, `-userdir` included, and the environment of the old one, which is how a Steam start keeps `SteamAppId`. The playset is the user folder's `dlc_load.json`, which the launcher writes before it starts the game and the new process reads again [code: Etch's corpus driver writes the same file to a `-userdir` profile]. Steam or GOG, launcher or a direct start: the restart does what **Back** without Hamweard did. [live] Checked on Steam with and without mods in the pull request; GOG has the same code path [pdb] and was not started.

## What happens to a campaign

[pdb] The game has two places that restart: the Exit to Menu branch of `CInGameIdler::Idle` (the one caller of `CApplication::Restart`) and `CGameSetup::Back`. The branch in `Idle` runs when `_bResignGame` is set, which `CQuitDialog::OnAccept`, `CEndGameConfirmation::OnAccept`, `CInGameIdler::EndGame` and `CIronmanLostCloudConnection::OnDecline` do, after the player's confirmation. It calls `NiceMultiplayerExit`, then `HandleIronmanSave`, then `Restart`. Setting the flag is therefore the game's Exit to Menu without the game's own dialogs: an ironman campaign is saved by the game's code, and a campaign that is not ironman is closed as the game closes it, which keeps nothing since the last save (manual or autosave).

Writing the application's flags during a campaign instead would skip `HandleIronmanSave`, and an ironman campaign would lose its progress since the last autosave. That is why a session takes the idler's flag and the front end the application's.

The request never loses a campaign silently: `restart` refuses a session unless the caller passes `ANVIL_GAME_RESTART_CLOSE_CAMPAIGN`, and anvil-game checks it again with the state of the frame it acts in, so a save that loaded between the click and the frame is not closed by a request made in the menu.

## Why in `anvil-game`

- **Not in the overlay.** The overlay draws on the render thread and has no game addresses; anvil-game already hooks `CApplication::UpdateOneFrame`, owns the application object of that hook, the session (the in-game idler, whose vtable it already checks) and the quit flags, and runs work on the main thread.
- **Not in Hamweard.** A restart is useful without Hamweard (a setting that applies at the next start, a mod author's change), and the overlay is Anvil's.
- **A public function, not a private channel between two standard plugins.** A service is how one plugin reaches another in API 2, and a plugin may need it too (a plugin of its own settings with `restart = true`). The rule for callers is in the header: only on the player's request, never on a timer or an event of the game.

## Considered options

- *Anvil starts a new `eu4.exe` and quits the old one.* Rejected: it would copy `PrepareCommandLineForRestart` (drop `continue`, pass the Paradox sign-in on Steam), race the old process for the game's files and the Steam session, and differ from the game's own restart in ways nobody would test.
- *Ask the Paradox launcher or Steam to start the game* (`steam://rungameid`). Rejected: the launcher may show its own window, the GOG build has no Steam, and the launch options and `-userdir` of the current start would be lost.
- *Write the application's flags in a campaign too*, after a confirmation. Rejected: it skips the ironman save (above).
- *Call `NiceMultiplayerExit` and `HandleIronmanSave` from anvil-game, then write the flags.* Rejected: it copies the game's branch outside the place it runs, with two more game functions per build, and would bypass Hamweard's clean-up of a return. The idler's flag lets the game run its own branch.
- *Refuse a campaign and send the player to Exit to Menu first.* Rejected: the button would not work from the screen where most players are. It stays the behaviour in multiplayer.
- *Hamweard asks anvil-game whether a restart is pending and restarts at once.* Not now: with Hamweard a restart from a campaign makes one return first (the reset takes a few seconds), which is correct and keeps the two plugins apart. It can come later as a field anvil-game appends.
- *Allow it in a multiplayer session.* Not in this version: the game's branch does leave the session with `NiceMultiplayerExit`, but nobody has tested it with a second player, and leaving a session from a panel button is easy to do by mistake.

## Consequences

- anvil-game declares 2 more symbols per build: the store of nation selection **Back** (its value is the flags' offset) and the check of `_bResignGame` in `CInGameIdler::Idle` (its value is the flag's offset). A build without them answers `ANVIL_ERROR_UNSUPPORTED`.
- `anvil_game.h` grows by one function and one flag, and `anvil-sdk` gets its wrapper in the same pull request (CONTRIBUTING, Rust parity).
- The overlay gets one button and its texts, in eight languages.
- [live] The new process starts while the old one is still quitting, so what one process holds alone is still held at the new one's start: Anvil's own `anvil.log` (opened for writing, shared for reading) and Tongs' pipe. Anvil keeps the new process's lines and opens the log again every 250 ms for up to 60 s (`anvil.log was still in use … opened N ms later`); Tongs waits for its pipe's name the same way (tongs#201). A plugin that opens a file or a named object of its own for the whole process has to expect the same.
- A restart takes as long as a start. The log says who asked and what anvil-game did (`restart requested by …`, `restart: the game's Exit to Menu`, `restart: application flags set`).

## Limits

- GOG was not started for this ADR or its pull request; its addresses come from the PDB and the bytes match the Steam code's shape.
- Multiplayer is refused, so the game's restart of a multiplayer session stays untested by Anvil.
- A request with Hamweard waits up to 120 s for the main menu after the session ends; a return that takes longer drops it with a warning. [hypothesis] No return in the lab took longer than about 6 s.
