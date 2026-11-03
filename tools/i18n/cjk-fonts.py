"""Writes the overlay's and the Manager's CJK fonts (src/i18n/fonts/README.md).

Each font is the subset of one Source Han Sans Regular face that holds the
characters its catalog uses (src/i18n/catalog_<code>.cpp) beyond the Latin
font: Simplified Chinese from the SC face, Traditional Chinese from TC,
Japanese from the default (J) face and Korean from K. The subsets are
Modified Versions under the SIL Open Font License, so they do not carry the
Reserved Font Name "Source": their family is "Anvil CJK <region>".

Run it after a CJK catalog changes; anvil_i18n_tests fails while a catalog
uses a character its font lacks.

    python -I tools/i18n/cjk-fonts.py <SourceHanSans.ttc>

The collection is SourceHanSans.ttc from 01_SourceHanSans.ttc.zip of the
2.005R release (adobe-fonts/source-han-sans), checked by SHA-256 below.
Needs fontTools (pip install fonttools==4.55.3).
"""

import hashlib
import pathlib
import re
import sys

from fontTools import subset
from fontTools.ttLib import TTCollection

TTC_SHA256 = "1382ea173f57af2ef9de32b41ce24d9c33fe69b8f4f2eae7cf05e64a4efd8aef"
ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "src" / "i18n" / "fonts"

# catalog, face index in the collection, region, output file
FONTS = [
    ("zh_hans", 17, "SC", "anvil-cjk-sc.otf"),
    ("zh_hant", 18, "TC", "anvil-cjk-tc.otf"),
    ("ja", 15, "JP", "anvil-cjk-jp.otf"),
    ("ko", 16, "KR", "anvil-cjk-kr.otf"),
]


def latin(cp: int) -> bool:
    """What the Latin font draws (the ranges of tests/i18n_tests.cpp in_fonts)."""
    return (0x20 <= cp <= 0x7E or 0xA0 <= cp <= 0x17F or cp in (0x2013, 0x2014, 0x2022, 0x2026)
            or 0x2018 <= cp <= 0x201E)


def catalog_characters(code: str) -> set[int]:
    text = (ROOT / "src" / "i18n" / f"catalog_{code}.cpp").read_text(encoding="utf-8")
    literals = re.findall(r'"((?:[^"\\]|\\.)*)"', text)
    chars = set()
    for literal in literals:
        chars.update(ord(c) for c in literal if not latin(ord(c)) and ord(c) >= 0x20)
    return chars


def rename(font, region: str) -> None:
    family = f"Anvil CJK {region}"
    postscript = f"AnvilCJK{region}-Regular"
    name = font["name"]
    for record in list(name.names):
        if record.nameID in (1, 16):
            record.string = family
        elif record.nameID in (2, 17):
            record.string = "Regular"
        elif record.nameID == 3:
            record.string = f"{postscript};2.005R subset"
        elif record.nameID == 4:
            record.string = f"{family} Regular"
        elif record.nameID == 6:
            record.string = postscript
    name.names = [r for r in name.names if r.nameID in (0, 1, 2, 3, 4, 5, 6, 13, 14, 16, 17)]
    cff = font["CFF "].cff
    cff.fontNames = [postscript]
    top = cff.topDictIndex[0]
    for key in ("FullName", "FamilyName"):
        if hasattr(top, key):
            setattr(top, key, f"{family} Regular" if key == "FullName" else family)
    if hasattr(top, "FDArray"):
        for fd in top.FDArray:
            if hasattr(fd, "FontName"):
                fd.FontName = fd.FontName.replace("SourceHanSans", f"AnvilCJK{region}")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    ttc = pathlib.Path(sys.argv[1])
    digest = hashlib.sha256(ttc.read_bytes()).hexdigest()
    if digest != TTC_SHA256:
        print(f"{ttc}: SHA-256 {digest}, expected {TTC_SHA256}")
        return 1
    collection = TTCollection(str(ttc), lazy=False)
    for code, index, region, file in FONTS:
        chars = catalog_characters(code)
        font = collection.fonts[index]
        options = subset.Options()
        options.layout_features = []
        options.hinting = False
        options.notdef_outline = True
        options.name_IDs = ["*"]
        options.name_languages = ["*"]
        options.drop_tables += ["VORG", "vhea", "vmtx", "BASE", "GDEF", "GSUB", "GPOS", "meta", "DSIG"]
        subsetter = subset.Subsetter(options)
        subsetter.populate(unicodes=sorted(chars))
        subsetter.subset(font)
        rename(font, region)
        out = OUT / file
        font.save(str(out))
        cmap = font.getBestCmap()
        missing = sorted(c for c in chars if c not in cmap)
        print(f"{file}: {len(chars)} characters, {font['maxp'].numGlyphs} glyphs, {out.stat().st_size} bytes"
              + (f"; not in the face: {' '.join(f'U+{c:04X}' for c in missing)}" if missing else ""))
        collection = TTCollection(str(ttc), lazy=False)  # subsetting changed the face in place
    return 0


if __name__ == "__main__":
    sys.exit(main())
