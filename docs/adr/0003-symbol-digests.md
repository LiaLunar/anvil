# Verify symbols by digest instead of publishing game bytes

Status: **accepted** (2026-11-04). `bytes` stays valid for all of API 2; the team's plugins move one by one (Migration).

`symbols.toml` states the expected bytes of each symbol in the open, so every public plugin repository, release zip and Workshop item carries short fragments of `eu4.exe`: about 13,400 bytes across the team's plugins. A plugin only needs to know that the bytes at an address are the right ones, which a digest can state without repeating them. This ADR adds a digest form next to `bytes` and keeps everything else about verification.

## Format

```toml
[symbol.speed_object]
rva = 0x8024D0
sha256 = "5F3A…"                                 # of the 8 bytes at rva, with the `any` offsets zeroed
size = 8                                         # required with sha256
any = [3, 4]                                     # optional: offsets not compared, as ?? in bytes
memory = "code"
values.speed = { at = 3, size = 4, is = 0x418 }  # optional is: the value the bytes must give
```

- **Digest:** upper- or lower-case hex of SHA-256 over `size` bytes, each `any` offset set to 0. A zeroed dump of those bytes gives the same digest in any tool.
- **Keys:** `sha256`, `size` and `any` are placement keys, like `bytes`: shared or per build. `bytes` and `sha256` exclude each other per placement. `any` needs at least one compared byte, as `bytes` needs one that is not `??`.
- **Values:** `at`, `size` and `rip_relative` keep their meaning. `is` states the result, which verification checks and which `anvil gen` writes as the constant. A value without `is` is read when the game loads.
- **What runs later:** once a symbol verifies, the loader gives its declaration the bytes it read from the image. Changes, `values`, instruction boundaries and crash reports work as they do with `bytes`.

## What stays

- The loader and `anvil check --exe` give the same verdicts: the plugin files of the team, converted, gave the same result as the originals on both builds, and a changed digest or `is` is reported.
- `symbols.lock` is unchanged. It digests `symbols.toml`, so a converted file needs `anvil check --exe` once per build.
- `anvil gen` output is the same when each constant has its `is`, which the converter writes.
- Cost: one digest takes about 1 µs through BCrypt, so all symbols of a large load order add under 1 ms.

## What is lost

- **The first differing byte.** A mismatch names both digests and the byte count, not where the bytes differ. A player's mismatch means another build or a changed exe either way. An author compares two exes with their own copies.
- **The pattern as a search key.** Neither `anvil check` nor the loader searches today; ports use the old exe and a disassembler. A port now needs the old `eu4.exe`, because the repository no longer holds the pattern. The digest can still confirm a candidate address, and by the 1 µs figure a sliding search over `.text` (29 MB) would take about 30 s per symbol.
- **Reading a pull request.** Bytes next to a comment could be checked by eye. With a digest only `anvil check --exe` can.
- **Tests that run declared bytes.** `anvil_game_plugin_tests` runs `plugins/anvil-game/symbols.toml`'s bytes on a stand-in image. It needs a synthetic stand-in, and a test `symbols.toml` it generates, before `anvil-game` can move.

## Compatibility

Within API 2 ([ADR 0001](0001-plugin-api-2-compatibility.md)): new keys, no new host field, and no change to generated code for an existing file.

- **Older plugins:** `bytes` stays valid for the whole of API 2. Nothing has to move.
- **Older Anvil:** it reads `sha256` as an unknown key and skips the plugin with that reason, as with `absent`, so a plugin that uses it declares `anvil = ">=<this release>"`.
- **Source:** a plugin that moves keeps compiling as long as its constants have `is`.

## Migration

1. Release Anvil with the digest form and `tools/symbols-to-digest.py`, which needs no game: it computes everything from the declared bytes.
2. For each plugin: run the tool, read the comments it leaves (a comment that spells out game code stays until someone removes it), run `anvil check --exe` once per build, raise `anvil`, and release.
3. Mei changes `tools/gen-symbols.py`, which writes its file. `anvil-game` moves last, after its test.
4. To add a symbol: write `bytes` locally and run the tool before committing. A later `anvil check --exe --fill` could write the digest directly.

## Considered options

- *Keep the bytes.* Rejected in this proposal: it keeps the copies public that this ADR exists to stop adding.
- *Trust the SHA-256 of the whole exe and verify nothing per symbol.* The file hash proves the build, not the memory: a third-party DLL can have changed the code before Anvil, and an author's wrong address would pass the loader. Rejected.
- *One digest per file.* Smaller, but a mismatch could not name the symbol, and `anvil check` could not report line by line. Rejected.
- *Keep bytes in a private file and generate the public one at build time.* Works for the team, but `symbols.toml` must stay readable and complete for third-party authors. Rejected as the format; an author may still work this way.

## Limits

A digest is not secrecy. Anyone with the game can recompute it, and a pattern of a few bytes can be recovered by brute force; Mei's median pattern is 5 bytes. The aim is that the repository holds no copy of game code, not that the address is hidden. Addresses, `size` and `is` values stay public, and so do git history, published releases and Workshop items, which this change does not touch.
