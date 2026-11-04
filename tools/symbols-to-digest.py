"""Rewrites a symbols.toml from `bytes` to `sha256`, `size` and `any`, in place.

Usage: python -I tools/symbols-to-digest.py <symbols.toml>...

It needs no game: the digest and the value of each `values` entry come from the
declared bytes, and `is` is written for a value that is the same in every build
the symbol is in. Everything else, comments included, stays as it was, so read
the comments afterwards: a comment that spells out game code is not rewritten.
Then run `anvil check --exe <eu4.exe>` once per build, which writes symbols.lock.
"""
import hashlib
import re
import sys
import tomllib
from pathlib import Path

BYTES = re.compile(r'\bbytes\s*=\s*"([0-9A-Fa-f? \t]+)"')
MULTILINE_BYTES = re.compile(r'(?m)^bytes\s*=\s*"""(.*?)"""', re.DOTALL)
VALUE = re.compile(r"^(\s*)((?:values\.)?(\w+))(\s*=\s*\{)(.*?)(\}.*)$")
TABLE = re.compile(r"^\s*\[+\s*symbol\.(\w+)(\.[^\]]*)?\]+")


def digest_keys(pattern):
    tokens = re.findall(r"\?\?|[0-9A-Fa-f]{2}", pattern)
    masked = bytes(0 if t == "??" else int(t, 16) for t in tokens)
    any_ = [i for i, t in enumerate(tokens) if t == "??"]
    return hashlib.sha256(masked).hexdigest().upper(), len(tokens), any_


def patterns(symbol):
    if "bytes" in symbol:
        yield symbol.get("rva"), symbol["bytes"]
    for table in symbol.get("build", {}).values():
        if "bytes" in table:
            yield table.get("rva", symbol.get("rva")), table["bytes"]


def value_in(rva, pattern, value):
    tokens = re.findall(r"\?\?|[0-9A-Fa-f]{2}", pattern)
    span = 4 if "rip_relative" in value else value["size"]
    raw = int.from_bytes(bytes(int(t, 16) for t in tokens[value["at"]: value["at"] + span]), "little")
    if "rip_relative" not in value:
        return raw
    if rva is None:
        return None  # an export: its rva is resolved when the game loads
    return rva + value["rip_relative"] + (raw - (1 << 32) if raw >= 1 << 31 else raw)


def constant(symbol, value):
    found = {value_in(rva, pattern, value) for rva, pattern in patterns(symbol)}
    return found.pop() if len(found) == 1 else None


def rewrite_multiline(match):
    sha256, size, any_ = digest_keys(match.group(1))
    return f'sha256 = "{sha256}"\nsize = {size}' + (f"\nany = {any_}" if any_ else "")


def rewrite_bytes(line):
    match = BYTES.search(line.split("#", 1)[0])
    if match is None:
        return line
    sha256, size, any_ = digest_keys(match.group(1))
    inline = "{" in line[: match.start()]
    separator = ", " if inline else "\n"
    keys = f'sha256 = "{sha256}"{separator}size = {size}'
    if any_:
        keys += f"{separator}any = {any_}"
    return line[: match.start()] + keys + line[match.end():]


def convert(text):
    symbols = tomllib.loads(text).get("symbol", {})
    out = []
    name = None
    in_values = False
    for line in text.split("\n"):
        code = line.split("#", 1)[0]
        table = TABLE.match(code)
        if table:
            name = table.group(1)
            in_values = (table.group(2) or "") == ".values"
        match = VALUE.match(line)
        if name and match and (match.group(2).startswith("values.") or in_values) and " at " in f" {match.group(5)}".replace("=", " "):
            value_name = match.group(3)
            value = symbols[name].get("values", {}).get(value_name)
            number = constant(symbols[name], value) if value else None
            if number is not None and "is =" not in match.group(5):
                body = match.group(5).rstrip()
                line = f"{match.group(1)}{match.group(2)}{match.group(4)}{body}, is = 0x{number:X} {match.group(6)}"
        out.append(rewrite_bytes(line))
    return MULTILINE_BYTES.sub(rewrite_multiline, "\n".join(out))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for name in sys.argv[1:]:
        path = Path(name)
        raw = path.read_bytes().decode("utf-8")
        crlf = "\r\n" in raw
        text = convert(raw.replace("\r\n", "\n"))
        tomllib.loads(text)
        path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))


if __name__ == "__main__":
    main()
