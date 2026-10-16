#!/usr/bin/env python3
"""Builds the emulator store assets and index.json (docs/PROTOCOL.md).

  tools/build_index.py --tag v2026.10.16.1 --stage build/stage --out dist
  tools/build_index.py --validate dist

--stage holds one folder per emulator, already populated by `make package`:
the executable, emulator.json and the license files. The manifest in the
stage folder is the one the player will read, so index fields come from it
and cannot drift from what is installed. dist/ gets every file under a flat
asset name <id>--<file>, index.json and SHA256SUMS. --validate re-checks a
finished dist/ (index rules plus every asset's size and SHA-256).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys

SCHEMA = 1
PROTOCOL = 1
# Used with fullmatch and re.ASCII so they mean what the player checks: a
# plain "$" would allow a trailing newline and "\d" any Unicode digit.
TAG_RE = re.compile(r"[A-Za-z0-9._-]{1,63}", re.ASCII)
ID_RE = re.compile(r"[a-z0-9][a-z0-9_-]{0,31}", re.ASCII)
VERSION_RE = re.compile(r"[0-9]+(\.[0-9]+){0,3}", re.ASCII)
ASSET_RE = re.compile(r"[A-Za-z0-9._-]+", re.ASCII)
FILE_NAME_RE = re.compile(r"[A-Za-z0-9._-]+", re.ASCII)
EXT_RE = re.compile(r"[a-z0-9]{1,7}", re.ASCII)
SHA_RE = re.compile(r"[0-9a-f]{64}", re.ASCII)
# The contract sets no per-file limit; these keep a release well inside any
# sane player limit and stop a runaway build.
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_INDEX_BYTES = 256 * 1024
MAX_EMULATORS = 64
MAX_FILES_PER_EMULATOR = 16


class BuildError(Exception):
    pass


def text(value, label, limit, required=True):
    if not isinstance(value, str) or len(value.encode("utf-8")) > limit or (required and not value.strip()):
        raise BuildError(f"{label} must be text of at most {limit} bytes")
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise BuildError(f"{label} must not contain control characters")
    return value


def plain_name(value, label):
    if not isinstance(value, str) or not value or "/" in value or value in (".", "..") or "\\" in value:
        raise BuildError(f"{label} {value!r} must be a plain name without '/'")
    return value


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def check_manifest(m, folder):
    """Checks emulator.json against the contract; returns it."""
    where = f"{folder}/emulator.json"
    if not isinstance(m, dict):
        raise BuildError(f"{where}: must be an object")
    if not isinstance(m.get("id"), str) or not ID_RE.fullmatch(m["id"]):
        raise BuildError(f"{where}: id must match {ID_RE.pattern}")
    if m["id"] != folder:
        raise BuildError(f"{where}: id {m['id']!r} differs from its folder {folder!r}")
    text(m.get("name"), f"{where}: name", 64)
    version = text(m.get("version"), f"{where}: version", 31)
    if not VERSION_RE.fullmatch(version):
        raise BuildError(f"{where}: version {version!r} must be dotted numbers, e.g. 1.0.0")
    if type(m.get("protocol")) is not int or m["protocol"] != PROTOCOL:
        raise BuildError(f"{where}: protocol must be {PROTOCOL}")
    plain_name(m.get("exec"), f"{where}: exec")
    text(m.get("description"), f"{where}: description", 400)
    text(m.get("author", ""), f"{where}: author", 64, required=False)
    text(m.get("license", ""), f"{where}: license", 64, required=False)
    systems = m.get("systems")
    if not isinstance(systems, list) or not 1 <= len(systems) <= 8:
        raise BuildError(f"{where}: systems must list 1 to 8 entries")
    for s in systems:
        if not isinstance(s, dict):
            raise BuildError(f"{where}: every system must be an object")
        text(s.get("name"), f"{where}: system name", 64)
        plain_name(s.get("games_dir"), f"{where}: games_dir")
        exts = s.get("extensions")
        if not isinstance(exts, list) or not 1 <= len(exts) <= 8:
            raise BuildError(f"{where}: extensions must list 1 to 8 entries")
        for e in exts:
            if not isinstance(e, str) or not EXT_RE.fullmatch(e):
                raise BuildError(f"{where}: extension {e!r} must be lowercase, without the dot, at most 7 bytes")
    return m


def check_entry(e, label):
    """Checks one index entry against the contract."""
    if not isinstance(e, dict):
        raise BuildError(f"{label}: must be an object")
    if not isinstance(e.get("id"), str) or not ID_RE.fullmatch(e["id"]):
        raise BuildError(f"{label}: bad id")
    eid = e["id"]
    text(e.get("name"), f"{label}: name", 64)
    if not VERSION_RE.fullmatch(text(e.get("version"), f"{label}: version", 31)):
        raise BuildError(f"{label}: bad version")
    if type(e.get("protocol")) is not int or e["protocol"] < 1:
        raise BuildError(f"{label}: bad protocol")
    text(e.get("description"), f"{label}: description", 400)
    text(e.get("author", ""), f"{label}: author", 64, required=False)
    text(e.get("license", ""), f"{label}: license", 64, required=False)
    systems = e.get("systems")
    if (not isinstance(systems, list) or not 1 <= len(systems) <= 8
            or any(not isinstance(s, str) or not s for s in systems)):
        raise BuildError(f"{label}: systems must list 1 to 8 names")
    files = e.get("files")
    if not isinstance(files, list) or not 1 <= len(files) <= MAX_FILES_PER_EMULATOR:
        raise BuildError(f"{label}: files must list 1 to {MAX_FILES_PER_EMULATOR} entries")
    total, dests = 0, set()
    for f in files:
        if not isinstance(f, dict):
            raise BuildError(f"{label}: every file must be an object")
        asset, dest = f.get("asset"), f.get("dest")
        if not isinstance(asset, str) or not ASSET_RE.fullmatch(asset) or len(asset) > 127:
            raise BuildError(f"{label}: bad asset name {asset!r}")
        if not isinstance(dest, str) or not dest.startswith(eid + "/") or len(dest) > 255:
            raise BuildError(f"{label}: dest {dest!r} must start with {eid}/")
        parts = dest.split("/")
        if any(p in ("", ".", "..") for p in parts) or "\\" in dest or any(ord(c) < 32 for c in dest):
            raise BuildError(f"{label}: dest {dest!r} must be a normalized relative path")
        if dest in dests:
            raise BuildError(f"{label}: duplicate dest {dest!r}")
        dests.add(dest)
        if not isinstance(f.get("sha256"), str) or not SHA_RE.fullmatch(f["sha256"]):
            raise BuildError(f"{label}: bad sha256 for {asset}")
        size = f.get("size")
        if type(size) is not int or not 0 < size <= MAX_FILE_BYTES:
            raise BuildError(f"{label}: {asset} size must be 1 to {MAX_FILE_BYTES} bytes")
        if "executable" in f and type(f["executable"]) is not bool:
            raise BuildError(f"{label}: executable must be true or false")
        total += size
    if type(e.get("size")) is not int or e["size"] != total:
        raise BuildError(f"{label}: size {e.get('size')!r} is not the sum of its files ({total})")
    if not any(f.get("executable") for f in files):
        raise BuildError(f"{label}: no executable file")
    return e


def check_index(index):
    if not isinstance(index, dict) or index.get("schema") != SCHEMA:
        raise BuildError(f"index: schema must be {SCHEMA}")
    if not isinstance(index.get("tag"), str) or not TAG_RE.fullmatch(index["tag"]):
        raise BuildError("index: bad tag")
    emulators = index.get("emulators")
    if not isinstance(emulators, list) or not 1 <= len(emulators) <= MAX_EMULATORS:
        raise BuildError("index: emulators must list 1 to %d entries" % MAX_EMULATORS)
    ids, assets = set(), set()
    for e in emulators:
        check_entry(e, f"emulators[{e.get('id') if isinstance(e, dict) else '?'}]")
        if e["id"] in ids:
            raise BuildError(f"index: duplicate id {e['id']}")
        ids.add(e["id"])
        for f in e["files"]:
            if f["asset"] in assets:
                raise BuildError(f"index: duplicate asset {f['asset']}")
            assets.add(f["asset"])


def build(tag, stage, out):
    if not TAG_RE.fullmatch(tag):
        raise BuildError(f"tag {tag!r} must be 1 to 63 characters of letters, digits, '.', '_' or '-'")
    folders = sorted(d for d in os.listdir(stage) if os.path.isdir(os.path.join(stage, d)))
    if not folders:
        raise BuildError(f"{stage}: no staged emulators (run `make package`)")
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    emulators, sums = [], []
    for folder in folders:
        base = os.path.join(stage, folder)
        manifest_path = os.path.join(base, "emulator.json")
        if not os.path.isfile(manifest_path):
            raise BuildError(f"{folder}: missing emulator.json")
        with open(manifest_path, encoding="utf-8") as f:
            m = check_manifest(json.load(f), folder)
        names = sorted(os.listdir(base))
        if m["exec"] not in names:
            raise BuildError(f"{folder}: executable {m['exec']!r} is not staged")
        files = []
        for name in names:
            path = os.path.join(base, name)
            if not os.path.isfile(path):
                raise BuildError(f"{folder}/{name}: only plain files can be packaged")
            if not FILE_NAME_RE.fullmatch(name):
                raise BuildError(f"{folder}/{name}: file name has characters GitHub would rename")
            size = os.path.getsize(path)
            if size == 0:
                raise BuildError(f"{folder}/{name}: empty file; the player refuses empty assets")
            asset = f"{folder}--{name}"
            item = {"asset": asset, "dest": f"{folder}/{name}", "sha256": sha256_of(path), "size": size}
            if name == m["exec"]:
                item["executable"] = True
            files.append(item)
            shutil.copyfile(path, os.path.join(out, asset))
            sums.append(f"{item['sha256']}  {asset}")
        # Executable first, as the contract's example lists it.
        files.sort(key=lambda i: (not i.get("executable"), i["asset"]))
        emulators.append({
            "id": m["id"],
            "name": m["name"],
            "version": m["version"],
            "protocol": m["protocol"],
            "systems": [s["name"] for s in m["systems"]],
            "description": m["description"],
            "author": m.get("author", ""),
            "license": m.get("license", ""),
            "size": sum(i["size"] for i in files),
            "files": files,
        })
    index = {"schema": SCHEMA, "tag": tag, "emulators": emulators}
    check_index(index)
    data = json.dumps(index, indent=2, ensure_ascii=False) + "\n"
    if len(data.encode("utf-8")) > MAX_INDEX_BYTES:
        raise BuildError("index.json is over %d bytes" % MAX_INDEX_BYTES)
    with open(os.path.join(out, "index.json"), "w", encoding="utf-8") as f:
        f.write(data)
    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="utf-8") as f:
        f.write("\n".join(sorted(sums, key=lambda s: s.split("  ", 1)[1])) + "\n")
    print(f"wrote {out}/ with {len(emulators)} emulator(s), tag {tag}")


def validate(dist):
    path = os.path.join(dist, "index.json")
    if os.path.getsize(path) > MAX_INDEX_BYTES:
        raise BuildError("index.json is over %d bytes" % MAX_INDEX_BYTES)
    with open(path, encoding="utf-8") as f:
        index = json.load(f)
    check_index(index)
    listed = {"index.json", "SHA256SUMS"}
    for e in index["emulators"]:
        for f in e["files"]:
            asset = os.path.join(dist, f["asset"])
            if not os.path.isfile(asset):
                raise BuildError(f"{f['asset']}: listed in the index but missing from {dist}")
            if os.path.getsize(asset) != f["size"] or sha256_of(asset) != f["sha256"]:
                raise BuildError(f"{f['asset']}: size or sha256 differs from the index")
            listed.add(f["asset"])
    extra = sorted(set(os.listdir(dist)) - listed)
    if extra:
        raise BuildError(f"{dist}: files not listed in the index: {', '.join(extra)}")
    print(f"{path}: valid, {len(index['emulators'])} emulator(s), tag {index['tag']}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tag", default=os.environ.get("TAG", "dev"), help="release tag (default $TAG or dev)")
    p.add_argument("--stage", default="build/stage", help="folder with one staged folder per emulator")
    p.add_argument("--out", default="dist", help="output folder")
    p.add_argument("--validate", metavar="DIST", help="only validate a finished dist folder")
    args = p.parse_args()
    try:
        if args.validate:
            validate(args.validate)
        else:
            build(args.tag, args.stage, args.out)
            validate(args.out)
    except BuildError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
