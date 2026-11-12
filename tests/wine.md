# Running the tests under Wine or Proton

The tests are built for Windows. They also run under Wine (Linux, Proton) with a cross-compiled build
(`clang-cl` and the Windows SDK from `xwin`), with the checks below adjusted. On Windows nothing changes: every
adjustment is guarded by `under_wine()` (`tests/test_support.hpp`), which reads Wine's own registry hive
(`HKCU\Software\Wine`) and so does not depend on the code under test.

`wine_skip(...)` prints `skipped under Wine: <what>` for each check that was not run, so the log says what was left
out. Each row below was probed on its own with a small Win32 program, not assumed.

| Test | What differs under Wine | How it was shown |
| --- | --- | --- |
| `anvil_workshop_tests`, "the folder hash with locks" | A folder that holds a file with an open, non-delete-sharing handle can be renamed (`MoveFileExW` succeeds). Windows refuses. The two checks after it only failed because the rename had moved the folder. | Open `GENERIC_READ`/`FILE_SHARE_READ`, then rename the folder: Wine allowed it. Write-open and `DeleteFile` were still refused, as on Windows. |
| `anvil_manager_tests`, "a folder the player cannot write offers the administrator" | `icacls /deny` succeeds but Wine does not enforce ACLs, so the write is never denied. | After `icacls /deny *S-1-1-0:(AD)`, creating a file in the folder succeeded. |
| `anvil_manager_tests`, "start menu: a shortcut ..." | `IShellLink::GetPath` returns the target folded to the ANSI code page (`ąć` becomes `ac`). The test uses a non-ASCII folder on purpose; under Wine it uses an ASCII one. | Wrote a link to a path with `ąć`, read it back: not equal. |
| `anvil_manager_tests`, "runner: WM_CLOSE after ready ..." | No Wine-specific change; it only needs a display (see below). | A child creating a window and a parent posting `WM_CLOSE`: works on Xvfb, fails without a display (`CreateWindowEx` returns null). The test passes unchanged, with its 100 ms delay, once a display exists. |
| `anvil_crash_tests`, "threads get a stack guarantee ..." | Wine gives every thread at least 1 MB of stack, so a thread with a 128 KB stack, which the check needs, cannot be made. | `CreateThread` with 64/128/256/1024 KB reservations: `GetCurrentThreadStackLimits` reports 1024 KB for all. |
| `anvil_crash_tests`, "a stack overflow writes the report and a minidump" | The report, dump and JSON are all written and checked as on Windows. Only the child's exit status differs: Wine ends it with its own status instead of `0xC0DE`/`EXCEPTION_STACK_OVERFLOW`. | Observed exit `0xD` with the report folder complete. |
| `anvil_tests`, "version resources and the log lines" | Wine's `d3d9.dll` carries Wine's version, not `10.0.`; `detect_wine()` is true. The test now expects `detect_wine() == under_wine()`. | Direct. |
| `anvil_sdk_cxx_only` | Not a Wine difference: the nested `cmake` configure did not receive the cross toolchain file or build type. `CMakeLists.txt` now forwards both when they are set (no effect for MSVC builds). | Direct. |

## `__try` / `/EHa` with clang-cl (build, not test)

MSVC honors `__try`/`__except` for hardware faults. `clang-cl` drops them from a function whose body has no calls
unless `/EHa` is given, and with `/EHa` a C++ `catch (...)` also swallows hardware faults, which MSVC's `/EHsc` never
does. The cross build therefore adds `/EHa` only when the file being compiled contains `__try`, `__except` or
`__finally` (see `sdk/cmake/toolchains/clang-cl-eha`, used by `sdk/cmake/toolchains/clang-cl-xwin.cmake`). Without it, `anvil_game_plugin_tests` crashed with an
unhandled page fault in `read_raw`, and every guarded read in the shipped plugins would have crashed the game
instead of being caught.

**Known divergence.** Four files that need `/EHa` also contain `catch (...)`: `plugins/anvil-game/src/plugin.cpp` (1),
`plugins/anvil-map/src/plugin.cpp` (6), `src/core/changes.cpp` (1) and, in Bellows, `src/modules/range_witness.cpp`
(5). In a clang-cl build those handlers also catch hardware faults (for example one inside another plugin's
callback), where an MSVC build lets the fault reach the crash handler. The long-term fix is to keep `__try` code in
its own translation unit, as `src/core/seh_guard.cpp` already does for the plugin load.

## Display

Tests that open windows need a display. The test script starts a private Xvfb with the GLX extension off (Xvfb
crashes at start-up in a sandbox with a minimal `/dev` otherwise).
