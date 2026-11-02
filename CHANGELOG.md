# Changelog

## [1.8.0](https://github.com/happy-dwarf-team/anvil/compare/v1.7.0...v1.8.0) (2026-11-02)


### Features

* show Anvil's version in the game's main menu and open the panel with a click ([#184](https://github.com/happy-dwarf-team/anvil/issues/184)) ([4d757a2](https://github.com/happy-dwarf-team/anvil/commit/4d757a2ae2734edad417573be86836d74bb8a43b))

## [1.7.0](https://github.com/happy-dwarf-team/anvil/compare/v1.6.1...v1.7.0) (2026-11-02)


### Features

* show player messages in the player's language ([#181](https://github.com/happy-dwarf-team/anvil/issues/181)) ([16d37cd](https://github.com/happy-dwarf-team/anvil/commit/16d37cdbb0bd7c97fd1d60fbfaf7fe1a475d5779))
* support the GOG build 1.37.5.2 and symbols for several builds ([#182](https://github.com/happy-dwarf-team/anvil/issues/182)) ([37350be](https://github.com/happy-dwarf-team/anvil/commit/37350be27a945410b2a27a130168b21af0c77d28))

## [1.6.1](https://github.com/happy-dwarf-team/anvil/compare/v1.6.0...v1.6.1) (2026-11-02)


### Bug Fixes

* **log-throttle:** name a message in anvil.log without its line break ([#176](https://github.com/happy-dwarf-team/anvil/issues/176)) ([f8673e6](https://github.com/happy-dwarf-team/anvil/commit/f8673e6b79919750082726040d5d78fc1fdaa701))

## [1.6.0](https://github.com/happy-dwarf-team/anvil/compare/v1.5.2...v1.6.0) (2026-11-01)


### Features

* **api:** offer the current game date in anvil-game.game ([#171](https://github.com/happy-dwarf-team/anvil/issues/171)) ([fdf3f4e](https://github.com/happy-dwarf-team/anvil/commit/fdf3f4e32c035c4c21bbc8f5c6b002c134784892))
* **log-throttle:** throttle repeated lines of the game's logs ([#173](https://github.com/happy-dwarf-team/anvil/issues/173)) ([6110566](https://github.com/happy-dwarf-team/anvil/commit/6110566324ed4faf4f1b5e8ca666e9ff1bb6a71d))

## [1.5.2](https://github.com/happy-dwarf-team/anvil/compare/v1.5.1...v1.5.2) (2026-10-26)


### Bug Fixes

* **core:** write the exit work from the game's ExitProcess ([#165](https://github.com/happy-dwarf-team/anvil/issues/165)) ([4155d4f](https://github.com/happy-dwarf-team/anvil/commit/4155d4f7614872b13a3c4af0d34ed61dc754d814)), closes [#164](https://github.com/happy-dwarf-team/anvil/issues/164)
* **game:** publish the session events of a save loaded from the game menu ([#169](https://github.com/happy-dwarf-team/anvil/issues/169)) ([13e5a4b](https://github.com/happy-dwarf-team/anvil/commit/13e5a4b7b0f1ab0e2517a8e7d5a7643f516eb365))

## [1.5.1](https://github.com/happy-dwarf-team/anvil/compare/v1.5.0...v1.5.1) (2026-10-26)


### Bug Fixes

* **manager:** open the privacy note on anvil.happydwarf.dev ([#156](https://github.com/happy-dwarf-team/anvil/issues/156)) ([a93a07e](https://github.com/happy-dwarf-team/anvil/commit/a93a07e97a00b651dec0f68200e707b8f11af951))

## [1.5.0](https://github.com/happy-dwarf-team/anvil/compare/v1.4.0...v1.5.0) (2026-10-26)


### Features

* **manager:** add the Benchmark tab ([#151](https://github.com/happy-dwarf-team/anvil/issues/151)) ([b0f2dae](https://github.com/happy-dwarf-team/anvil/commit/b0f2daef32f401f5f7792bb2276618c5220ceaa7))
* **manager:** run a benchmark series of game starts ([#149](https://github.com/happy-dwarf-team/anvil/issues/149)) ([aa867f3](https://github.com/happy-dwarf-team/anvil/commit/aa867f34d7a7d8c4b2dc7bf9efd66c9e5a28cedb))
* **manager:** send a benchmark report to the receiver ([#152](https://github.com/happy-dwarf-team/anvil/issues/152)) ([bc8ce69](https://github.com/happy-dwarf-team/anvil/commit/bc8ce693c5253afef5b567ce336180bd5f2b44d8))
* **manager:** write and keep the benchmark report format 1 ([#148](https://github.com/happy-dwarf-team/anvil/issues/148)) ([168744b](https://github.com/happy-dwarf-team/anvil/commit/168744b7c8f8c2f862e28336190bd03c338c7a6d))

## [1.4.0](https://github.com/happy-dwarf-team/anvil/compare/v1.3.0...v1.4.0) (2026-10-25)


### Features

* **api:** add the anvil.benchmark v1 service for plugin fields ([#138](https://github.com/happy-dwarf-team/anvil/issues/138)) ([12386e7](https://github.com/happy-dwarf-team/anvil/commit/12386e7861b4ed64eba7784191a163cc344336b7))
* **core:** read the benchmark arm and leave the compared plugin out ([#141](https://github.com/happy-dwarf-team/anvil/issues/141)) ([f751cff](https://github.com/happy-dwarf-team/anvil/commit/f751cff8b9274d69b26e2dd2b8f73a7b70192842))
* **core:** write the benchmark start record with open cost and labels ([#142](https://github.com/happy-dwarf-team/anvil/issues/142)) ([f26c978](https://github.com/happy-dwarf-team/anvil/commit/f26c97803ac7f062af4e38fdeb88008ae8bcefc0))
* **game:** quit a benchmark start once every participant is done ([#143](https://github.com/happy-dwarf-team/anvil/issues/143)) ([e5559b3](https://github.com/happy-dwarf-team/anvil/commit/e5559b3c8a7ccb8b29fe27fd2da8f0a0cbbf9325))
* **manager:** classify benchmark starts and compute the series result ([#147](https://github.com/happy-dwarf-team/anvil/issues/147)) ([9e97c38](https://github.com/happy-dwarf-team/anvil/commit/9e97c38c40b8f1f5c4a11a14d68317d9be12600f))

## [1.3.0](https://github.com/happy-dwarf-team/anvil/compare/v1.2.1...v1.3.0) (2026-10-20)


### Features

* **core:** discover plugins in enabled mods and load approved ones ([#113](https://github.com/happy-dwarf-team/anvil/issues/113)) ([74fce2d](https://github.com/happy-dwarf-team/anvil/commit/74fce2d96c1a3a7eb0584fd0c01786e88ca4fb49))
* **manager:** approve, revoke and show plugins from mods ([#116](https://github.com/happy-dwarf-team/anvil/issues/116)) ([2484cc2](https://github.com/happy-dwarf-team/anvil/commit/2484cc219a5091eb081ca3ab2d6d26df5ce2daab))
* **overlay:** approve and revoke plugins from mods in the panel ([#114](https://github.com/happy-dwarf-team/anvil/issues/114)) ([e825771](https://github.com/happy-dwarf-team/anvil/commit/e8257713824c1b5f9bedf61ccbbcad47bb6a422d))


### Bug Fixes

* **crash:** report a stack overflow on the game's other threads where Windows enables AMX ([#119](https://github.com/happy-dwarf-team/anvil/issues/119)) ([baf9bc8](https://github.com/happy-dwarf-team/anvil/commit/baf9bc8784c438be53249236aba08ea058fb64cd))

## [1.2.1](https://github.com/happy-dwarf-team/anvil/compare/v1.2.0...v1.2.1) (2026-10-20)


### Bug Fixes

* **manager:** accept posted mouse messages from automated tests ([#111](https://github.com/happy-dwarf-team/anvil/issues/111)) ([11243d0](https://github.com/happy-dwarf-team/anvil/commit/11243d0bd5d69988668074af00d648d3aa087bfa))
* **manager:** remove an empty anvil/crashes folder on uninstall ([#110](https://github.com/happy-dwarf-team/anvil/issues/110)) ([245935e](https://github.com/happy-dwarf-team/anvil/commit/245935e273c7c0f9358077309314f4fe06282d31))
* release the standard plugins at Anvil's version ([#109](https://github.com/happy-dwarf-team/anvil/issues/109)) ([be510e8](https://github.com/happy-dwarf-team/anvil/commit/be510e89b5c2defb550611080f8082987761ce23))

## [1.2.0](https://github.com/happy-dwarf-team/anvil/compare/v1.1.0...v1.2.0) (2026-10-20)


### Features

* **manager:** add AnvilManager.exe, the player's installer and management tool ([#101](https://github.com/happy-dwarf-team/anvil/issues/101)) ([5f57b1a](https://github.com/happy-dwarf-team/anvil/commit/5f57b1a0079326df8ad3e436f5ad6ce77275cf57))
* **manager:** add the Manager's core with game detection, install plans and plugin zips ([#100](https://github.com/happy-dwarf-team/anvil/issues/100)) ([a0981fa](https://github.com/happy-dwarf-team/anvil/commit/a0981faa4fb0ed402c95702d441582664c3297a4))
* **release:** ship AnvilManager.exe as an asset and in the Anvil zip ([#102](https://github.com/happy-dwarf-team/anvil/issues/102)) ([ef139a9](https://github.com/happy-dwarf-team/anvil/commit/ef139a9d0d77c154d1c0fee6373bd7e826dfe493))

## [1.1.0](https://github.com/happy-dwarf-team/anvil/compare/v1.0.0...v1.1.0) (2026-10-20)


### Features

* **core:** add the private anvil.control service for the overlay ([#93](https://github.com/happy-dwarf-team/anvil/issues/93)) ([46f611e](https://github.com/happy-dwarf-team/anvil/commit/46f611e40fab81f3b8edc5fe37d6ec76dc5fb199))
* **overlay:** add the anvil-overlay standard plugin ([#96](https://github.com/happy-dwarf-team/anvil/issues/96)) ([bb19b7a](https://github.com/happy-dwarf-team/anvil/commit/bb19b7a0dfeab2d75e9596f36e7dfa4c4eae9984))


### Bug Fixes

* **core:** report a crash in a mid hook's or block replacement's callback ([#95](https://github.com/happy-dwarf-team/anvil/issues/95)) ([3d292de](https://github.com/happy-dwarf-team/anvil/commit/3d292deca5e72b469ab0355e3b18d4deda9eceb1))


### Performance

* **core:** share one context stub so mid hooks and block replacements fit 1 KB ([#97](https://github.com/happy-dwarf-team/anvil/issues/97)) ([3ccf840](https://github.com/happy-dwarf-team/anvil/commit/3ccf840845168b01c6451ada3ffcafc0ce6c820c)), closes [#90](https://github.com/happy-dwarf-team/anvil/issues/90)
* **core:** trim the heap of every change, most of all mid hooks and block replacements ([#92](https://github.com/happy-dwarf-team/anvil/issues/92)) ([c4582f3](https://github.com/happy-dwarf-team/anvil/commit/c4582f33a881bdaf0d430774db3e903ff9d5830d)), closes [#90](https://github.com/happy-dwarf-team/anvil/issues/90)

## [1.0.0](https://github.com/happy-dwarf-team/anvil/compare/v0.2.0...v1.0.0) (2026-10-20)


### ⚠ BREAKING CHANGES

* **api:** plugin API 1 is removed. Plugins include the new anvil_plugin.h (or anvil.hpp), declare every eu4.exe address in symbols.toml and pass symbol ids (SYM_* / sym::*) instead of RVAs with expected bytes; hook_function, redirect_call and replace_import return their own `next`; expect_bytes, alloc_near_image, ANVIL_EVENT_GAME_READY and the PE fields of AnvilGameInfo are gone; result codes are renumbered.
* **core:** changes an API 1 plugin requests in anvil_plugin_load are enabled only after it returns ANVIL_OK. A second hook, redirect or import replacement at the same address joins a chain instead of failing with ANVIL_ERROR_CONFLICT, expected bytes are compared with the original bytes, and an overlap with the plugin's own change at another address is refused.
* **manifest:** plugin.toml must declare api = 2 and affects_gameplay, and must not use game_builds, [[expect]] or any other unknown key. Plugins with api = 1 are skipped.

### Features

* **anvil-game:** readiness, frame, main-thread work and the gameplay policy ([#77](https://github.com/happy-dwarf-team/anvil/issues/77)) ([376bc82](https://github.com/happy-dwarf-team/anvil/commit/376bc82a70f6c3e64117bd2f2151b5145a9bc7c0)), closes [#54](https://github.com/happy-dwarf-team/anvil/issues/54)
* **api:** plugin API 2 header, C++ wrapper and hello examples ([#74](https://github.com/happy-dwarf-team/anvil/issues/74)) ([5ba11a1](https://github.com/happy-dwarf-team/anvil/commit/5ba11a193802f973effd7100b6ed24b49971abeb)), closes [#46](https://github.com/happy-dwarf-team/anvil/issues/46)
* **cli:** anvil check --game ([#68](https://github.com/happy-dwarf-team/anvil/issues/68)) ([ab63642](https://github.com/happy-dwarf-team/anvil/commit/ab6364209627634d6b5669d6edb5946395db83fa))
* **cli:** anvil gen and anvil check ([#67](https://github.com/happy-dwarf-team/anvil/issues/67)) ([63da5a4](https://github.com/happy-dwarf-team/anvil/commit/63da5a4c18c8bb364c184abae15fe5e2535cdc9e)), closes [#41](https://github.com/happy-dwarf-team/anvil/issues/41)
* **core:** change registry with Anvil-owned chains, staging and handles ([#66](https://github.com/happy-dwarf-team/anvil/issues/66)) ([245591c](https://github.com/happy-dwarf-team/anvil/commit/245591c8539c0741ea3c601a5384fbad182d1c09))
* **core:** crash capture and attribution with safe mode ([#79](https://github.com/happy-dwarf-team/anvil/issues/79)) ([7fae2b5](https://github.com/happy-dwarf-team/anvil/commit/7fae2b57a7b092b6106f55d88437eadcb8e91a49))
* **core:** mid hooks with AnvilCpuContext and block replacement ([#69](https://github.com/happy-dwarf-team/anvil/issues/69)) ([6988aef](https://github.com/happy-dwarf-team/anvil/commit/6988aef9eaef1594517a16d1f4893443b5b1e04b)), closes [#43](https://github.com/happy-dwarf-team/anvil/issues/43)
* **core:** optional per-change call counters ([#73](https://github.com/happy-dwarf-team/anvil/issues/73)) ([1440cd5](https://github.com/happy-dwarf-team/anvil/commit/1440cd57d4ae664fc8ca25f4052d450fed3964ac))
* **core:** plugin settings ([#78](https://github.com/happy-dwarf-team/anvil/issues/78)) ([893b3ce](https://github.com/happy-dwarf-team/anvil/commit/893b3ce258e270d6eb5760c1a2e7138f5cdba8cb)), closes [#48](https://github.com/happy-dwarf-team/anvil/issues/48)
* **core:** pointer swap ([#71](https://github.com/happy-dwarf-team/anvil/issues/71)) ([c3d7c0f](https://github.com/happy-dwarf-team/anvil/commit/c3d7c0febe229873a75b2791555a2b30130e942d))
* **core:** skip plugins that declare a conflict with an enabled plugin ([#65](https://github.com/happy-dwarf-team/anvil/issues/65)) ([f657df5](https://github.com/happy-dwarf-team/anvil/commit/f657df5175c823c92a7423656c35264129989f66))
* **core:** suspend and resume all changes of a plugin ([#72](https://github.com/happy-dwarf-team/anvil/issues/72)) ([9dc06ce](https://github.com/happy-dwarf-team/anvil/commit/9dc06ce117391054fc099220d652300ea7c7eeb2))
* **manifest:** manifest v2 ([#62](https://github.com/happy-dwarf-team/anvil/issues/62)) ([800232b](https://github.com/happy-dwarf-team/anvil/commit/800232bbc4a45f6a42f25ef565c6538b54a5df97))
* **rust:** anvil-sys, anvil gen --lang rust and a Rust hello ([#76](https://github.com/happy-dwarf-team/anvil/issues/76)) ([ab8f7e6](https://github.com/happy-dwarf-team/anvil/commit/ab8f7e699b408e94f117f2b8d5f89e19773d0114))
* **symbols:** parse and verify symbols.toml ([#64](https://github.com/happy-dwarf-team/anvil/issues/64)) ([8b8b50f](https://github.com/happy-dwarf-team/anvil/commit/8b8b50fce06f6abce5fae940d331b487f583368e))


### Bug Fixes

* **build:** let the SDK from source work without the C language ([#85](https://github.com/happy-dwarf-team/anvil/issues/85)) ([51c8ac4](https://github.com/happy-dwarf-team/anvil/commit/51c8ac46350f2598c800c7f52bf85d21ec163b39)), closes [#81](https://github.com/happy-dwarf-team/anvil/issues/81)
* **cli:** check value names the generators reject ([#84](https://github.com/happy-dwarf-team/anvil/issues/84)) ([f56b442](https://github.com/happy-dwarf-team/anvil/commit/f56b442287c6901b493ad3bb7975ac40e9bbd397)), closes [#80](https://github.com/happy-dwarf-team/anvil/issues/80)
* **core:** keep the safe-mode count and finish the minidump after a crash ([#83](https://github.com/happy-dwarf-team/anvil/issues/83)) ([13db38d](https://github.com/happy-dwarf-team/anvil/commit/13db38d7aeefd731c6b468844a16f30b67b1de61)), closes [#82](https://github.com/happy-dwarf-team/anvil/issues/82)
* **examples:** add the symbols.lock of hello-rust ([#88](https://github.com/happy-dwarf-team/anvil/issues/88)) ([e38435e](https://github.com/happy-dwarf-team/anvil/commit/e38435eee4d4aae795c91ff8c4c951a0f96394d6)), closes [#86](https://github.com/happy-dwarf-team/anvil/issues/86)


### Build

* add the SDK archive and CMake package ([#75](https://github.com/happy-dwarf-team/anvil/issues/75)) ([cc84234](https://github.com/happy-dwarf-team/anvil/commit/cc84234980b6e02649b2a68c7befffb6de793063)), closes [#53](https://github.com/happy-dwarf-team/anvil/issues/53)


### Documentation

* give mid hooks and block replacements a 2 KB memory budget ([#89](https://github.com/happy-dwarf-team/anvil/issues/89)) ([f25b1d7](https://github.com/happy-dwarf-team/anvil/commit/f25b1d703262668beb76bfd4a1b3407ef7dd7f08)), closes [#57](https://github.com/happy-dwarf-team/anvil/issues/57)

## [0.2.0](https://github.com/happy-dwarf-team/anvil/compare/v0.1.0...v0.2.0) (2026-10-15)


### Features

* **api:** verify dependency bytes before patching ([#1](https://github.com/happy-dwarf-team/anvil/issues/1)) ([b78072d](https://github.com/happy-dwarf-team/anvil/commit/b78072df7c38374024526a2858fe8ca8ad899ea5))
* **manifest:** let plugins require a minimum Anvil version ([#3](https://github.com/happy-dwarf-team/anvil/issues/3)) ([668ff79](https://github.com/happy-dwarf-team/anvil/commit/668ff79d9c8a5617c7848b3bca8bdcd738346def))


### Build

* read the version from version.txt only ([#2](https://github.com/happy-dwarf-team/anvil/issues/2)) ([f24ae77](https://github.com/happy-dwarf-team/anvil/commit/f24ae770930af446c490f83b53b096d949982e07))
