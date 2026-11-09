# AnvilManager.exe

The player's installer and management tool (design §15). `src/` is the
testable core (`anvil_manager_tests`), `app/` the window: Dear ImGui on
Direct3D 11.

## Driving it from automated tests

Tests can run install, plugin zips and uninstall with window messages only,
without focus and without moving the cursor (issue #106):

- The Manager speaks the language of EU4's settings, or the Chinese,
  Japanese or Korean of Steam or Windows, so labels and their
  positions differ by language. Start it with `ANVIL_LANGUAGE=en` in its
  environment for the English layout, or point `userdir.txt` in the copy at
  a folder whose `settings.txt` sets `language` to test another one.
- Start it on a copy of a game folder: `AnvilManager.exe --game <copy>`. It
  remembers the folder in `%LOCALAPPDATA%\Anvil\manager.toml`; remove that file
  after the test.
- Click with posted messages to its window: `WM_MOUSEMOVE`, then
  `WM_LBUTTONDOWN` and `WM_LBUTTONUP`, with `lParam` in client coordinates
  (physical pixels: the Manager is per-monitor DPI aware). Wait about 100 ms
  after the move: tabs act only on an item that was hovered in the frame
  before. A posted mouse message is recognised by a cursor position
  (`GetMessagePos`) other than its `lParam`, and keeps its position until the
  real mouse moves over the window again.
- The window may be behind other windows or in front; it must not be
  minimized, because the frame loop skips frames while `Present` reports the
  window as occluded.
- Install, uninstall and plugin zips ask in a dialog drawn by ImGui; click its
  button the same way. Take a fresh capture before every click, because the
  layout moves with the state (for example a note about another Manager
  version in the folder). `PrintWindow` with `PW_CLIENTONLY |
  PW_RENDERFULLCONTENT` captures the client area without bringing it to the
  front.
- "Install a plugin from a zip..." opens the system's open-file dialog (a
  `#32770` window of the same process). Set the path with `WM_SETTEXT` on the
  `Edit` inside the file name box (control id `0x47C`) and post `WM_COMMAND`
  `IDOK` to the dialog.
- The Benchmark tab without the game: copy `anvil_manager_tests.exe` as
  `eu4.exe` into the folder. Started with `ANVIL_BENCHMARK` in its
  environment, it plays a game that writes the core's start record with a
  stub participant and quits after 2 s (`fake_benchmark_game` in
  `tests/manager_benchmark_tests.cpp`). The folder's `eu4.exe` is then an
  unknown build, so `anvil-game` does not load and the setup's checks fail:
  a real series needs the game.
