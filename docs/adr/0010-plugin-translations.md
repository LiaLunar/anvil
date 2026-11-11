# Plugin translations: `translations/<code>.toml` in the plugin package, looked up by the English words

Status: **accepted** (2026-11-11, the maintainer's decision in [eu4-engine-lab#113](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/113)). Built in Anvil 1.20 ([eu4-engine-lab#159](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/159)).

The overlay and the Manager speak eight languages besides English ([#158](https://github.com/happy-dwarf-team/anvil/issues/158), design §15), but the texts of a plugin stayed as its author wrote them: the description in `plugin.toml` and the labels, descriptions, groups and option labels in `settings.toml`. So did the summary and the note of each entry of the plugin catalog (ADR 0002). A Chinese player who installs Mei through a Chinese Manager met Mei's nine settings in English. This ADR decides where the translations of those texts live, how a text finds its translation and what checks them. The file format is hard to change once plugins ship it.

## Decision

1. **A plugin translation is a file of the plugin package:** `translations/<code>.toml` next to `plugin.toml`, one per language, with the codes of `ANVIL_LANGUAGE`: `fr`, `de`, `es`, `zh-Hans`, `zh-Hant`, `ja`, `ko`, `pl`. English has no file: `plugin.toml` and `settings.toml` are the source. The plugin versions and releases its translations with its texts.
2. **No new key in `plugin.toml` or `settings.toml`.** Both files refuse unknown keys, and an older Anvil skips a plugin whose file it cannot read (as with `absent`, design §5.2). A folder no older Anvil reads changes nothing there: the plugin shows English and loads as before, so a plugin adds translations without raising its `anvil` range.
3. **A text is looked up by its English words**, as the catalogs of Anvil's own texts are (`src/i18n/catalog_*.cpp`):

   ```toml
   status = "draft"            # "checked" once a native speaker checked it
   [text]
   "Panel key" = "Touche du panneau"
   "Opens and closes the Anvil panel." = "Ouvre et ferme le panneau d'Anvil."
   ```

   When the author changes an English text, its old translation no longer matches and the player sees the new English words, not a translation of what the text used to say; `anvil check` names the old entry. A text used twice (a group, an option label such as "Off") is translated once.
4. **What is translated:** `description` of `plugin.toml`; `label`, `description`, `group` and the option labels of `settings.toml`. A plugin's `name` is a name and stays. `anvil.log`, `anvil check` and the benchmark reports stay English, as Anvil's own do.
5. **A text without a translation is shown in English**, with no mark, as in Anvil's own catalogs. So is every text of a plugin without a file for the language.
6. **The catalog's texts live in the catalog:** a new field of an entry, `"translations": { "<code>": { "summary": "…", "note": "…" } }`. `schema` stays 1: an older Manager ignores a field it does not know, while a `summary` of another type would make it refuse the entry. The `catalog` job of CI requires both texts in every language.
7. **Chinese, Japanese and Korean come from the fonts of Windows.** Anvil's font subsets hold only the characters of its own catalogs, and a plugin is released apart from Anvil. The overlay merges the fonts of Windows once a plugin text in one of these languages is shown (as it already did for panels), and the Manager merges the player's language's font of Windows next to the subset.
8. **`anvil check` checks the folder:** a file that does not parse, a file name that is not `<code>.toml` of a known code, and an entry whose English words are no text of the plugin are errors; a text without a translation, and a language without a file, are warnings, which `--complete-translations` makes errors. The loader never skips a plugin for its translations: it leaves a broken file out with a warning in `anvil.log`.
9. **Anvil's own plugins use the same files:** `plugins/<id>/translations/` for the five standard plugins, checked with `--complete-translations` in `ctest`. `anvil_i18n_tests` keeps to the texts of Anvil itself.

## Considered options

- *New keys in `settings.toml` (`label_fr = "…"`, or a `[setting.x.translations]` table).* Rejected: every plugin with a translation would need an Anvil that knows the key, or be skipped by older ones, and the settings file would grow eight times for one language's player.
- *All translations in the anvil repository* (one table per plugin, or Anvil's catalogs). Rejected: a plugin's release would wait for an Anvil release to show its new texts translated, translations would drift from the plugin's versions, and a plugin of another author would have no way in. The draft of the script errors panel (anvil#318) translated `anvil-log-throttle`'s settings through Anvil's catalogs; with this decision they move to the plugin's own files.
- *Keys by the setting's name* (`[setting.mode] label = "…"`). Rejected: it survives a fixed typo, but after the English meaning changes the player keeps seeing the old translation, and nothing can tell.
- *Marking machine translations in the interface.* Rejected for now: the overlay and the Manager already show Anvil's own drafts unmarked. The state is in the file (`status`), for translators.

## Consequences

- `anvil.control` gains `plugin_text` and `mod_plugin_text` (appended in 1.20). The core reads the player's language's file of each resolved plugin and each plugin in a mod once at start.
- `anvil_add_plugin` copies `translations/*.toml` into the built plugin folder; the release layout installs those of the standard plugins.
- The team's plugins add the eight files in their next regular release, and `anvil-plugin-template` carries them with `--complete-translations` in CI ([eu4-engine-lab#160](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/160), [#161](https://github.com/happy-dwarf-team/eu4-engine-lab/issues/161)).
- The agent drafts every language in the pull request that adds or changes a text; the maintainer checks Polish in the review. The other languages stay `draft` until a native speaker checks them (eu4-engine-lab#24); a pull request that changes a `checked` file of another language sets it back to `draft`.
