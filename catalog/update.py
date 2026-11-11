"""Write and check catalog/plugins.json, the Manager's plugin catalog (ADR 0002).

    python catalog/update.py <id> [--tag vX.Y.Z]   pin the latest (or that) release of a plugin
    python catalog/update.py --check               verify every pin, as CI does

Both download the pinned zip, compare its size and SHA-256, verify its build
provenance with `gh attestation verify --repo happy-dwarf-team/<repo>`, and
compare plugin.toml and symbols.toml in the zip with the entry. They need the
GitHub CLI (`gh`), signed in or with GH_TOKEN set, and Python 3.11 or newer.

Each entry's summary and note need `translations` in every language the
Manager shows (design §15, ADR 0010); the check names what is missing.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import tomllib
import urllib.request
import zipfile

CATALOG = pathlib.Path(__file__).with_name("plugins.json")
OWNER = "happy-dwarf-team"
DOWNLOAD_PREFIX = f"https://github.com/{OWNER}/"
MAX_ZIP_BYTES = 256 << 20  # the Manager's limit for a plugin zip
# The languages of the Manager besides English, as ANVIL_LANGUAGE writes them.
LANGUAGES = ["fr", "de", "es", "zh-Hans", "zh-Hant", "ja", "ko", "pl"]


def gh(*args: str) -> str:
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


def repo_of(entry: dict) -> str:
    url = entry["zip"]["url"]
    if not url.startswith(DOWNLOAD_PREFIX) or "/releases/download/" not in url:
        raise SystemExit(f"{entry['id']}: the zip is not a release asset of {OWNER}: {url}")
    return url[len(DOWNLOAD_PREFIX):].split("/", 1)[0]


def download(url: str, limit: int) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "anvil-catalog-check"})
    with urllib.request.urlopen(request, timeout=120) as reply:
        data = reply.read(limit + 1)
    if len(data) > limit:
        raise SystemExit(f"{url}: more than {limit} bytes")
    return data


def manifest_of(data: bytes) -> tuple[dict, dict | None]:
    """plugin.toml and symbols.toml of the shallowest plugin folder in the zip, as the Manager finds it."""
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = [n for n in archive.namelist() if not n.endswith("/")]
        manifests = sorted((n for n in names if n.rsplit("/", 1)[-1] == "plugin.toml"), key=lambda n: n.count("/"))
        if not manifests:
            raise SystemExit("the zip has no plugin.toml")
        root = manifests[0][: -len("plugin.toml")]
        manifest = tomllib.loads(archive.read(manifests[0]).decode("utf-8"))
        symbols_name = root + manifest.get("symbols", "")
        symbols = tomllib.loads(archive.read(symbols_name).decode("utf-8")) if "symbols" in manifest else None
    return manifest, symbols


def builds_of(symbols: dict | None) -> list[str]:
    if symbols is None:
        return []  # no symbols: every build
    if "builds" in symbols:
        return list(symbols["builds"])
    return [symbols["build"]]


def translation_problems(entry: dict) -> list[str]:
    """What the entry's `translations` lack: every language needs the summary and, with one, the note."""
    problems: list[str] = []
    translations = entry.get("translations", {})
    if not isinstance(translations, dict):
        return ["translations is not an object of languages"]
    for code in translations:
        if code not in LANGUAGES:
            problems.append(f"translations.{code}: not a language of the Manager ({', '.join(LANGUAGES)})")
    for code in LANGUAGES:
        words = translations.get(code)
        if not isinstance(words, dict):
            problems.append(f"translations.{code} is missing")
            continue
        for key in words:
            if key not in ("summary", "note"):
                problems.append(f"translations.{code}.{key}: only summary and note are translated")
        for key in ("summary", "note"):
            if entry.get(key) and not (isinstance(words.get(key), str) and words[key].strip()):
                problems.append(f"translations.{code}.{key} is missing")
            if not entry.get(key) and key in words:
                problems.append(f"translations.{code}.{key}: the entry has no {key}")
    return problems


def verify(entry: dict) -> list[str]:
    """What is wrong with one entry; empty when its pin holds."""
    problems: list[str] = []
    repo = repo_of(entry)
    pin = entry["zip"]
    if not 0 < pin["size"] <= MAX_ZIP_BYTES:
        return [f"size {pin['size']} is not between 1 and {MAX_ZIP_BYTES}"]
    data = download(pin["url"], pin["size"])
    if len(data) != pin["size"]:
        problems.append(f"size is {len(data)}, the pin says {pin['size']}")
    digest = hashlib.sha256(data).hexdigest()
    if digest != pin["sha256"]:
        problems.append(f"SHA-256 is {digest}, the pin says {pin['sha256']}")
        return problems
    with tempfile.TemporaryDirectory() as temp:
        file = pathlib.Path(temp) / pin["url"].rsplit("/", 1)[-1]
        file.write_bytes(data)
        attested = subprocess.run(["gh", "attestation", "verify", str(file), "--repo", f"{OWNER}/{repo}"],
                                  capture_output=True, text=True)
        if attested.returncode != 0:
            problems.append(f"no build provenance of {OWNER}/{repo}: {attested.stderr.strip() or attested.stdout.strip()}")
    manifest, symbols = manifest_of(data)
    expected = {
        "id": manifest.get("id"),
        "version": manifest.get("version"),
        "anvil": manifest.get("anvil", ""),
        "affects_gameplay": manifest.get("affects_gameplay", False),
        "builds": builds_of(symbols),
    }
    for key, value in expected.items():
        if entry.get(key) != value:
            problems.append(f"{key} is {entry.get(key)!r}, the zip's manifest says {value!r}")
    return problems


def pin(entry: dict, tag: str | None) -> None:
    repo = entry["repo"].rstrip("/").rsplit("/", 1)[-1]
    release = json.loads(gh("release", "view", *([tag] if tag else []), "-R", f"{OWNER}/{repo}", "--json", "tagName,assets"))
    assets = [a for a in release["assets"] if a["name"] == entry["zip"]["url"].rsplit("/", 1)[-1]]
    if len(assets) != 1:
        raise SystemExit(f"{repo} {release['tagName']}: no asset named like the current pin")
    asset = assets[0]
    data = download(asset["url"], MAX_ZIP_BYTES)
    manifest, symbols = manifest_of(data)
    entry["version"] = manifest["version"]
    entry["anvil"] = manifest.get("anvil", "")
    entry["affects_gameplay"] = manifest.get("affects_gameplay", False)
    entry["builds"] = builds_of(symbols)
    entry["zip"] = {"url": asset["url"], "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("id", nargs="?", help="the plugin to pin to a new release")
    parser.add_argument("--tag", help="the release to pin (default: the latest)")
    parser.add_argument("--check", action="store_true", help="verify every pin")
    args = parser.parse_args()
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    if catalog.get("schema") != 1:
        raise SystemExit("catalog/plugins.json: schema is not 1")
    if args.id:
        entries = [e for e in catalog["plugins"] if e["id"] == args.id]
        if not entries:
            raise SystemExit(f"{args.id} is not in the catalog")
        pin(entries[0], args.tag)
        CATALOG.write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"{args.id}: pinned {entries[0]['version']}")
    elif not args.check:
        parser.error("give a plugin id or --check")
    failed = False
    for entry in catalog["plugins"] if args.check or not args.id else entries:
        problems = translation_problems(entry) + verify(entry)
        print(f"{entry['id']} {entry['version']}: {'ok' if not problems else 'FAIL'}")
        for problem in problems:
            print(f"  {problem}")
        failed |= bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
