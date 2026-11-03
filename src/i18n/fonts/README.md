# The CJK fonts of the overlay and the Manager

The overlay and the Manager draw Latin text with Segoe UI from Windows (or ImGui's own
font). Chinese, Japanese and Korean come from these four fonts, which `fonts.rc` links into
`anvil_overlay.dll`, `AnvilManager.exe` and `anvil_i18n_tests.exe` (design §15). Only the
player's language's font is loaded, and only the characters its catalog uses go into the
atlas, merged into the Latin font.

| File | Language | Face of Source Han Sans 2.005R |
| --- | --- | --- |
| `anvil-cjk-sc.otf` | Simplified Chinese (`catalog_zh_hans.cpp`) | SC Regular |
| `anvil-cjk-tc.otf` | Traditional Chinese (`catalog_zh_hant.cpp`) | TC Regular |
| `anvil-cjk-jp.otf` | Japanese (`catalog_ja.cpp`) | J Regular |
| `anvil-cjk-kr.otf` | Korean (`catalog_ko.cpp`) | K Regular |

Each is a subset with the characters of its catalog beyond Latin Extended-A (a few hundred,
35–115 KB), without hinting and OpenType layout, which ImGui does not use. A region's own
face gives each language its own forms of the ideographs it shares with the others.

## Source and licence

The fonts are subsets of [Source Han Sans](https://github.com/adobe-fonts/source-han-sans)
by Adobe, under the SIL Open Font License 1.1 (`OFL.txt`, installed as
`anvil/docs/third-party/SourceHanSans-OFL.txt`; the Manager's About tab names it). A subset
is a Modified Version, which may not use the Reserved Font Name "Source": their family is
"Anvil CJK SC", "Anvil CJK TC", "Anvil CJK JP" and "Anvil CJK KR", and `anvil_i18n_tests`
checks that their names do not contain it. The copyright notice stays in each font.

## When a CJK catalog changes

`anvil_i18n_tests` fails while a catalog uses a character its font does not have. Write the
fonts again from `SourceHanSans.ttc` of the release's `01_SourceHanSans.ttc.zip`, which the
script checks by SHA-256 (fontTools 4.55.3):

```
pip install fonttools==4.55.3
python -I tools/i18n/cjk-fonts.py <folder>/SourceHanSans.ttc
```

The script prints each font's characters, glyphs and size; commit the fonts with the catalog.
