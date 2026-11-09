# The game tells the player about a newer Anvil, opt-in, through the overlay

More players use Anvil, and most of them start the game far more often than Anvil Manager. Until now they learned about a new Anvil only from the Manager's opt-in release check, which shows a link on its Anvil tab, or from outside Anvil (the Discord's #releases). A player who installed Anvil once and plays from Steam never sees either. `anvil-overlay` therefore gets the same release check: once per start, in the background, and a notice with a link when a newer release exists. Design §19 said "Anvil and its standard plugins never download anything" and only the Manager asked GitHub, so this is a change of the security rules.

- **What goes to the network.** One HTTPS GET of `api.github.com/repos/happy-dwarf-team/anvil/releases/latest` per game start, sent as `anvil-overlay/<version>`, read as JSON (`tag_name` and `html_url` only, at most 1 MiB). Nothing is downloaded, installed or run. The notice links only to a page under `github.com/happy-dwarf-team/anvil/releases/`; any other URL in the reply refuses the whole reply. Updating Anvil stays what it was: run the newer `AnvilManager.exe`.
- **One check, shared code.** The Manager and the overlay call the same `anvil_net` library (`src/net`): the GET, the parsing, the version comparison and the reading of the Manager's answer. The Manager's own copy of that code is gone.
- **When.** When the game is ready (`anvil-game.ready`, the main menu), on a thread of its own: the game's threads never wait for the network, and the start-up the benchmark measures is over by then. `winhttp.dll` (and `shell32.dll`, `ole32.dll`) are delay-loaded into `anvil_overlay.dll`, whose static imports stay `KERNEL32` and `USER32` (EU4 1.37.5 loads all three itself: `eu4.exe` imports `shell32` and `ole32`, its EOS SDK `winhttp`); `winhttp.dll` is loaded from System32 only. Without the network the GET fails, the log says so, and nothing is shown. A start of the Manager's start-up benchmark (`ANVIL_BENCHMARK` set) never checks.
- **Consent.** The overlay's setting `release_check` (Check for new Anvil releases): `ask` (default), `on`, `off`.
  - `ask` follows the player's answer to the Manager's question (`check_releases` once `asked_releases` is true in `%LOCALAPPDATA%\Anvil\manager.toml`). Without one, the overlay asks once, in a notice in the main menu, with Yes and No; the answer becomes the setting. Closing the question without an answer asks again at the next start.
  - The Manager, when it never asked the player, takes the answer from the overlay's setting in the chosen game folder (`on` or `off`) instead of asking. So one answer counts for both, and each tool's own explicit choice wins over the other's.
  - The Manager's question and its About checkbox now say "when the Manager or the game starts", since a yes there turns the game's check on too. A player who said yes in an older Manager said it only for the Manager; the overlay follows it all the same, and its notice says the panel's settings turn the check off.
- **How often it bothers the player.** The notice of a release shows at each start, for the notification time, until the player closes it with its × or opens the release page; after that the same release is not shown again (`anvil/data/anvil-overlay/release-notice.txt` keeps its tag), and the next one is. The panel's Status tab names a newer release whenever the check found one. The notice shows only with the setting `notifications` on.

## Considered options

- *Leave it to the Manager.* Rejected: the players who most need to hear about an update are those who never open it again.
- *Check by default, without asking.* Rejected: a request to GitHub tells it the player's address and that they play now with Anvil; the Manager asked first for this reason, and the game should not be less careful than the Manager.
- *Ask in the game separately from the Manager.* Rejected: a player who already answered in the Manager would be asked again, and two settings that mean the same thing drift apart.
- *Have the core (`version.dll`) check.* Rejected: the core has no UI and runs before the game; the check belongs where the notice is, and `version.dll` keeps no network code at all.
- *Download and install the update from the game.* Rejected for the same reasons ADR 0002 gave for the latest release, and because the game holds `version.dll` open while it runs.
- *Cache the answer for a day.* Not done: one small request per start is what the Manager does too, well inside GitHub's limits for unauthenticated requests (60 an hour per address).

## Consequences

- Design §15 and §19 say the overlay may ask `api.github.com` for the latest release, opt-in; `version.dll`, `anvil-game` and `anvil-log-throttle` still never use the network.
- The overlay gains a way to open a link: "Open its page" starts the browser through `ShellExecuteW` on a thread of its own, and only for a release page of this repository. "Copy link" does what it does in the crash tab.
- The overlay's notices and the Manager's question use translated texts in all seven catalogs; they are machine-drafted until a native speaker checks them.
- A new release reaches players who opted in at their next game start; those who did not answer are asked once in the game.
