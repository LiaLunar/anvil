# Plugin API 2 stays compatible for the whole of an Anvil major version

Plugins outside the team now build on API 2 (the Unicode Patch and Menu Patch ports, plugins on the Workshop), and library plugins such as Tongs are about to publish services of their own, so "append-only" has to cover more than the host table. We decided on one contract for everything a plugin can see, whose short form is in [CONTRIBUTING.md](../../CONTRIBUTING.md):

- **Binary.** `AnvilHost` grows only after the "end of API 2.0" line and manifest keys are only added, as before. Every **service**, Anvil's and any plugin's alike, is a name and a **service version**: its table starts with `uint32_t size` and only grows at the end within that version, and a publisher may register several versions of one name at once, so it can offer the old and the new one while consumers move.
- **Lifetime.** In API 2 nothing is unloaded: plugins, their services, registered callbacks and every pointer the host or a service hands out stay valid until the process ends. This is what the code already does; writing it down means consumers never handle a publisher that disappears.
- **Behaviour.** What a header documents (the thread a callback runs on, the order of events, result codes) belongs to the contract as much as the layout. Changing it with the same layout needs a new service version, or a new API version for the host. What a header does not promise is not part of the contract.
- **Source.** `anvil.hpp`, `anvil_symbols.hpp`, the output of `anvil gen` and `anvil-sys` follow Anvil's SemVer: within Anvil 1.x a plugin that compiled keeps compiling. Breaking them needs `!` and a new Anvil major version. The safe Rust crate `anvil-sdk` has its own version line and joins this rule at its 1.0, which comes after the first real Rust plugin; until then a new 0.x minor version may break it.
- **Leaving API 2.** API 2 has no end date. An API 3 comes only for a change that cannot be appended, together with a new Anvil major version, and that major version still loads `api = 2` plugins throughout. API 1 had no such bridge; there were no plugins outside the team then.

## Considered options

- *Allow breaking source changes in a minor release.* Rejected: a plugin author updating the SDK would meet compile errors without a major version telling them to expect it.
- *Hot reload as a developer mode.* Rejected for API 2 by the lifetime rule: unloading a publisher would need consumers to hold services through weak references, as Reloaded-II does, and every pointer the host hands out would need an owner. It stays out until an API 3.
- *A cut without a bridge, as from API 1 to API 2.* Rejected: players would lose plugins whose authors have not migrated yet.

## Consequences

- `anvil.control` stays private and outside this contract; it may change in a minor release.
- A library plugin that changes a service in a breaking way publishes a new version next to the old one instead of replacing it.
