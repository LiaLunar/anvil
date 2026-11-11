# PROTOTYPE: script profiler (eu4-engine-lab#154)

Throwaway. Branch `prototype/script-profiler`, never merged, never released.
It answers the questions of eu4-engine-lab#154 before the profiler's shape is
decided (eu4-engine-lab#158).

## What it does

- Detours `CAndTrigger::Evaluate` (Steam 1.37.5 `0xB98200`), which evaluates
  every AND block: the root of each trigger (a `CRootTrigger` member of an
  event, decision, triggered modifier, ...), each `limit` and each `AND =`.
- Detours `CEffect::Execute` (`0x5ED800`), which runs every effect block.
- Per thread, only the outermost block (a *root*) is timed with `rdtsc`,
  inclusive of what it calls; inner blocks are only counted. Effects and
  triggers have separate depths, so a `limit` inside an effect is a root
  trigger whose parent is that effect.
- A root is keyed by its address. Its owner is found when it is shown: the
  block is a member at a known offset of its owner (`src/owners.inc`, made
  from the GOG 1.37.5.2 PDB layouts), and the owner's vtable must be that
  class's. eu4.exe is built without RTTI for its own classes, so a class is
  told by slots 0 and 1 of its vtable as Steam RVAs (`src/vtables.inc`, 3476
  classes from the GOG PDB and the lab's GOG-to-Steam function map; 209
  slot pairs are shared by several classes after identical-code folding,
  shown with `~`). The name is the owner's `_DebugKey`, `_Tag`, `_Key` or
  `_Name`.
- Optionally counts each log line written while a root runs on the same
  thread for that root (the file sink slot that `anvil-log-throttle` swaps).
- Panel `Script profiler (PROTOTYPE)` with Start, Stop, Reset and Write CSV;
  CSV in `logs/script_profile_<tag>.csv` of the user folder.

## Levels

Setting `mode` (at start) or `<userdir>/script_profiler_control.txt` with
`<level> <tag>` (read every 2 s): 0 removes the hooks, 1 installs them and
passes on, 2 counts roots, 3 counts and times them. A change writes the span
before it to the old tag's CSV and starts a new one at level 2 or 3.

## Run

`src/owners.inc` and `src/vtables.inc` are generated from the GOG PDB and
are not in Git: the lab keeps them and their generators (private). Without
them the plugin does not build.

Build Anvil as usual (the prototype is added in the top-level
`CMakeLists.txt`), then install `build/plugin/prototype-script-profiler`
next to `anvil-overlay` and `anvil-game`, e.g. with the lab's
`scripts/anvil-play.ps1 -ExtraPlugins`.
