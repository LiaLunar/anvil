# Contributing to Anvil

## Pull requests

Every change goes to `main` through a pull request; nobody pushes to `main`
directly. A pull request needs the `build` check (build, tests, package) and the
`pr-title` check to pass, and is **squash-merged**: the pull request title
becomes the single commit on `main` and, through it, the changelog entry. Keep
one topic per pull request; split unrelated changes.

## Commit and pull request titles

Titles follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<optional scope>): <summary in the imperative, lowercase, no period>
```

| Type       | Use for                                             | Release   | Changelog     |
| ---------- | --------------------------------------------------- | --------- | ------------- |
| `feat`     | new capability for players or plugin authors        | minor     | Features      |
| `fix`      | bug fix                                             | patch     | Bug Fixes     |
| `perf`     | faster or lighter, same behaviour                   | patch     | Performance   |
| `revert`   | reverting an earlier change                         | patch     | Reverts       |
| `build`    | CMake, dependencies, packaging, version resource    | patch     | Build         |
| `docs`     | documentation only                                  | none      | hidden        |
| `refactor` | code change without behaviour change                | none      | hidden        |
| `test`     | tests only                                          | none      | hidden        |
| `ci`       | GitHub Actions, Dependabot                          | none      | hidden        |
| `chore`    | anything else (release pull requests use it)        | none      | hidden        |

release-please proposes a release only when the changelog would get a visible
entry, so commits of hidden types alone never start one; they ship with the
next release.

Scopes in use: `api` (plugin SDK / `AnvilHost`), `manifest`, `core` (loader,
resolver, change registry), `proxy` (`version.dll` exports, entry hook), `log`,
`cli` (`anvil.exe`), `bench-receiver` (the start-up benchmark receiver in
`services/bench-receiver/`).
A scope is optional; leave it out when a change spans several.

release-please ships nothing from `services/`: a change there alone uses a
hidden type (`chore(bench-receiver): …`), unless it comes with the Manager
change it belongs to.

Breaking changes: add `!` after the type or scope (`feat(api)!: …`) and a
`BREAKING CHANGE: <what breaks and how to migrate>` paragraph in the pull
request description. Before 1.0.0 a breaking change bumps the minor version.

The plugin API is a separate contract from the Anvil version, and it covers
everything a plugin can see:

- Within plugin API 2, host functions and fields are only **appended** after
  the "end of API 2.0" line of `anvil_plugin.h` (plugins detect them with
  `ANVIL_HOST_HAS`) and manifest keys only added. That is a
  `feat`, and plugins that need it declare `anvil = ">=x.y.z"`. Unknown
  manifest keys are errors, so an older Anvil skips a plugin that uses a newer
  key.
- Removing or changing anything a plugin can see increments
  `ANVIL_API_VERSION` and is a breaking change (`feat(api)!`).
- A service, Anvil's or a plugin's, is a name and a major version. Its
  table starts with `uint32_t size` and only grows at the end within that
  version; a breaking change is a new version, which the publisher may
  register next to the old one.
- Nothing is unloaded in API 2: plugins, services, callbacks and every
  pointer the host or a service returns stay valid until the process ends.
- Behaviour a header documents (threads, event order, result codes) is part
  of the contract like the layout; changing it needs a new version.
- `anvil.hpp`, `anvil_symbols.hpp`, `anvil gen` output and `anvil-sys` keep
  source compatibility within an Anvil major version.
- An API 3 comes only with a new Anvil major version, which still loads
  `api = 2` plugins. The reasons are in
  [ADR 0001](docs/adr/0001-plugin-api-2-compatibility.md).
- `sdk/rust/anvil-sys/src/bindings.rs` is generated from `anvil_plugin.h` and
  committed. After a change to the header run `sdk/rust/regen-bindings.ps1`
  (bindgen and libclang, see the script) and commit the result; CI fails when
  the file differs from what the header gives.

## Versions and releases

Releases are automated with
[release-please](https://github.com/googleapis/release-please):

1. Each push to `main` updates a pull request titled
   `chore(main): release x.y.z`. It bumps `version.txt` and adds the
   `CHANGELOG.md` section, both computed from the Conventional Commits since
   the last release.
2. Merging that pull request tags `vX.Y.Z` and creates a **draft** GitHub
   release with the changelog section as release notes.
3. The release workflow then builds the tagged commit, attaches
   `anvil-windows-x64.zip`, `AnvilManager.exe`, `anvil-sdk-windows-x64.zip` and
   `SHA256SUMS.txt` with a build provenance attestation to the draft, and
   publishes it: the release is never public without its files, and nobody has
   to click. It also publishes the `anvil-sys` crate to crates.io (only when the
   `CARGO_REGISTRY_TOKEN` repository secret exists; otherwise it warns), and asks
   anvil.happydwarf.dev and happydwarf.dev to rebuild once the release is public
   (`website.yml`, which can also be run by hand). If the build or the upload
   fails, the draft stays a draft: rerun the failed jobs, or publish it by hand
   once its files are attached.

`version.txt` is the only place the version is written by hand. CMake, the
`anvil.log` line and the DLL version resource all read it, and release-please
copies it to `sdk/rust/anvil-sys/Cargo.toml`. Do not edit `version.txt` or
`CHANGELOG.md` by hand; to force a specific next version, put
`Release-As: x.y.z` in the body of a commit on `main`.

## Building and testing

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

A change that touches game memory or the loader should also be tried on a copy
of the game; say in the pull request what was and was not tested.

A change to stubs, chains or the core start-up also runs the microbenchmarks
(design §17). CI runs them and fails when a case is more than 20% slower than
`bench/baseline-ci.json` on the same runner CPU:

```powershell
build/Release/anvil_bench.exe --out bench-1.json
bench/check-regression.ps1 -Result bench-1.json
```

Binding numbers come from the lab machine: `bench/run-lab.ps1`.

A change that adds memory per change or to the core start-up also measures the
memory budgets (design §17). CI only runs this; it does not gate on the bytes:

```powershell
build/Release/anvil_bench.exe --memory --out bench-memory.json
bench/run-lab.ps1 -Memory   # exit 1 over a budget
```

## Texts for players and translations

The overlay and the Manager show their texts in the player's language (design
§15). A text the player reads goes through `tr("…")`, or `trf("… {0} …", {arg})`
when it has arguments (`src/i18n/translate.hpp`); a text picked from several
(a ternary, a table) is marked with `N_("…")` where it is written. Build
sentences whole, with placeholders, rather than from pieces, since word order
differs between languages. `anvil.log`, `anvil check` and the benchmark
reports stay English.

A reason the core gives (why a plugin is skipped or failed) is a message by
code: add the code and its English words to `src/i18n/message.cpp` and build
it with `i18n::Message("code", {args})`.

Every new or changed English text needs its entry in each catalog,
`src/i18n/catalog_fr.cpp`, `catalog_de.cpp` and `catalog_es.cpp`.
`anvil_i18n_tests` (in `ctest`) names what is missing or unused, a translation
whose placeholders differ, and a character the fonts do not have;
`build/Release/anvil_i18n_tests.exe --keys` prints every text a catalog needs.
A translation is checked by a native speaker before it counts as done; the
pull request says which entries are not checked yet. To see a language without
changing EU4's settings, start the game or the Manager with `ANVIL_LANGUAGE`
set to `en`, `fr`, `de` or `es`.

## Benchmark receiver

`services/bench-receiver/` is the Cloudflare Worker that takes the start-up
benchmark reports (spec section 9). Its README covers the code, deployment and
reading reports. CI runs its tests in the `bench-receiver` workflow, without a
Cloudflare secret; locally, with Node.js 22 or newer:

```powershell
cd services/bench-receiver
npm ci
npm test
npm run check
```

The report format's JSON Schema in `services/bench-receiver/schemas/` is the
contract between the Manager and the receiver: one pull request changes both,
and the Manager's tests validate their reports against it.

### A release that changes the report format

A release whose Manager sends a new report format, or the release that first
sends format 1 (Anvil 1.5), is merged only after these steps:

1. The pull request that changes the Manager's report also adds the format's
   schema, example, checks, views and tests to `services/bench-receiver/`
   (its README, "Report formats"), and is merged on `main`.
2. A maintainer applies the new migrations and deploys the receiver from
   `main` (README, "Deployment").
3. A test report of the new format is sent to
   `https://bench.happydwarf.dev/reports` and answered 201, then deleted by
   hand.
4. `docs/benchmark-privacy.md` says what the new format adds, its copy on
   the website (the Manager's privacy link,
   `https://anvil.happydwarf.dev/docs/benchmark-privacy/`, from
   `anvil-website`) says the same, and `privacy@happydwarf.dev` still
   forwards mail.
5. The release pull request is merged.

Old formats stay accepted; retiring one is a change of its own.
