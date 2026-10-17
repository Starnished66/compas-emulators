#!/usr/bin/env python3
"""Builds the emulator store assets and index.json (docs/PROTOCOL.md).

  tools/build_index.py --tag v2026.10.16.1 --stage build/stage --out dist
  tools/build_index.py --validate dist
  tools/build_index.py --check-manifests emulators

--stage holds one folder per emulator, already populated by `make package`:
the executable, emulator.json, the license files and, under games/<games_dir>/,\nthe games bundled with the emulator. The manifest in the
stage folder is the one the player will read, so index fields come from it
and cannot drift from what is installed. dist/ gets every file under a flat
asset name <id>--<file> (a nested file games/Doom/x.wad becomes <id>--games--Doom--x.wad), index.json and SHA256SUMS. --validate re-checks a
finished dist/ (index rules plus every asset's size and SHA-256).
--check-manifests checks the emulator.json of every folder under the given
directory without building anything.

The limits below are the ones the player enforces (compas-player
src/core/emulator_registry.c, src/network/emulator_store.c), which are
stricter than docs/PROTOCOL.md in places: system id <= 15 bytes, system name
<= 32 bytes, manifest <= 16 KiB, index <= 64 KiB, <= 32 emulators, <= 16 files
and <= 64 MiB per file, at most 96 bytes of joined system names are shown.
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
# The player's limits (STORE_FILE_LIMIT, 64 MiB since the player that also lists bundled games, STORE_INDEX_LIMIT, MANIFEST_LIMIT,
# EMULATOR_STORE_MAX_EMULATORS, EMULATOR_STORE_MAX_FILES).
MAX_FILE_BYTES = 64 << 20
MAX_INDEX_BYTES = 64 * 1024
MAX_MANIFEST_BYTES = 16 * 1024
MAX_EMULATORS = 32
MAX_FILES_PER_EMULATOR = 16
MAX_SYSTEM_NAMES_SHOWN = 96
SYSTEM_ID_RE = re.compile(r"[A-Za-z0-9_-]{1,15}", re.ASCII)
BUTTON_NAMES = {"a", "b", "x", "y", "l", "r", "select", "start"}


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
    text(m["exec"], f"{where}: exec", 63)
    text(m.get("description"), f"{where}: description", 400)
    text(m.get("author", ""), f"{where}: author", 64, required=False)
    text(m.get("license", ""), f"{where}: license", 64, required=False)
    systems = m.get("systems")
    if not isinstance(systems, list) or not 1 <= len(systems) <= 8:
        raise BuildError(f"{where}: systems must list 1 to 8 entries")
    for s in systems:
        if not isinstance(s, dict):
            raise BuildError(f"{where}: every system must be an object")
        if not isinstance(s.get("id"), str) or not SYSTEM_ID_RE.fullmatch(s["id"]):
            raise BuildError(f"{where}: system id must match {SYSTEM_ID_RE.pattern}")
        text(s.get("name"), f"{where}: system name", 32)
        text(s.get("games_dir"), f"{where}: games_dir", 63)
        plain_name(s.get("games_dir"), f"{where}: games_dir")
        if " " in s["games_dir"]:
            raise BuildError(f"{where}: games_dir {s['games_dir']!r} must not contain spaces")
        exts = s.get("extensions")
        if not isinstance(exts, list) or not 1 <= len(exts) <= 8:
            raise BuildError(f"{where}: extensions must list 1 to 8 entries")
        for e in exts:
            if not isinstance(e, str) or not EXT_RE.fullmatch(e):
                raise BuildError(f"{where}: extension {e!r} must be lowercase, without the dot, at most 7 bytes")
    if len({s["id"] for s in systems}) != len(systems):
        raise BuildError(f"{where}: duplicate system id")
    if len(", ".join(s["name"] for s in systems).encode("utf-8")) > MAX_SYSTEM_NAMES_SHOWN:
        raise BuildError(f"{where}: system names joined are over {MAX_SYSTEM_NAMES_SHOWN} bytes (the store truncates them)")
    check_frontend(m.get("frontend"), where)
    return m


def check_frontend(fe, where):
    """The optional "frontend" object read by the emulator's own frontend (frontend/config.h)."""
    if fe is None:
        return
    if not isinstance(fe, dict):
        raise BuildError(f"{where}: frontend must be an object")
    buttons = fe.get("buttons")
    if buttons is not None and (not isinstance(buttons, list) or not buttons or
                                any(b not in BUTTON_NAMES for b in buttons)):
        raise BuildError(f"{where}: frontend.buttons must list names from {sorted(BUTTON_NAMES)}")
    options = fe.get("options")
    if options is not None:
        if not isinstance(options, dict) or len(options) > 32:
            raise BuildError(f"{where}: frontend.options must be an object of at most 32 entries")
        for k, v in options.items():
            if len(k.encode()) > 63 or isinstance(v, (dict, list)) or len(str(v).encode()) > 63:
                raise BuildError(f"{where}: frontend option {k!r} is too long or not a plain value")
    if fe.get("frameskip", "off") not in ("off", "auto", "half"):
        raise BuildError(f"{where}: frontend.frameskip must be off, auto or half")
    if not isinstance(fe.get("audio_stretch", False), bool):
        raise BuildError(f"{where}: frontend.audio_stretch must be true or false")
    skip_options = fe.get("skip_options")
    if skip_options is not None:
        if not isinstance(skip_options, dict) or len(skip_options) > 32:
            raise BuildError(f"{where}: frontend.skip_options must be an object of at most 32 entries")
        for k, v in skip_options.items():
            if len(k.encode()) > 63 or isinstance(v, (dict, list)) or len(str(v).encode()) > 63:
                raise BuildError(f"{where}: frontend skip option {k!r} is too long or not a plain value")


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
    if any(len(s.encode("utf-8")) > 32 for s in systems):
        raise BuildError(f"{label}: a system name is over 32 bytes")
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


def staged_files(base, folder):
    """Relative paths ('/' separated) of the files of one staged emulator. Files sit
    directly in the folder, or under games/<games_dir>/ for games bundled with the
    emulator (the player lists those); nothing else may be nested."""
    names = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, base).replace(os.sep, "/")
        rel_dir = "" if rel_dir == "." else rel_dir
        depth = len(rel_dir.split("/")) if rel_dir else 0
        for d in dirnames:
            if not FILE_NAME_RE.fullmatch(d) or os.path.islink(os.path.join(dirpath, d)):
                raise BuildError(f"{folder}/{rel_dir}/{d}: bad folder name")
            if (depth == 0 and d != "games") or depth > 1:
                raise BuildError(f"{folder}/{rel_dir}/{d}: only games/<games_dir>/ may be nested")
        for name in sorted(filenames):
            rel = f"{rel_dir}/{name}" if rel_dir else name
            if not os.path.isfile(os.path.join(dirpath, name)) or os.path.islink(os.path.join(dirpath, name)):
                raise BuildError(f"{folder}/{rel}: only plain files can be packaged")
            if not FILE_NAME_RE.fullmatch(name):
                raise BuildError(f"{folder}/{rel}: file name has characters GitHub would rename")
            if rel_dir and not (depth == 2 and rel_dir.startswith("games/")):
                raise BuildError(f"{folder}/{rel}: bundled games belong in games/<games_dir>/")
            names.append(rel)
    return sorted(names)


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
        names = staged_files(base, folder)
        if m["exec"] not in names:
            raise BuildError(f"{folder}: executable {m['exec']!r} is not staged")
        files = []
        for name in names:
            path = os.path.join(base, *name.split("/"))
            if name.startswith("games/") and name.split("/")[1] not in {x["games_dir"] for x in m["systems"]}:
                raise BuildError(f"{folder}/{name}: {name.split('/')[1]!r} is not a games_dir of this emulator")
            size = os.path.getsize(path)
            if size == 0:
                raise BuildError(f"{folder}/{name}: empty file; the player refuses empty assets")
            asset = f"{folder}--{name.replace('/', '--')}"
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


def check_manifests(root):
    folders = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)))
    if not folders:
        raise BuildError(f"{root}: no emulator folders")
    games = {}
    for folder in folders:
        path = os.path.join(root, folder, "emulator.json")
        if not os.path.isfile(path):
            raise BuildError(f"{folder}: missing emulator.json")
        if os.path.getsize(path) > MAX_MANIFEST_BYTES:
            raise BuildError(f"{path}: over {MAX_MANIFEST_BYTES} bytes")
        with open(path, encoding="utf-8") as f:
            m = check_manifest(json.load(f), folder)
        if m["exec"] != folder:
            raise BuildError(f"{path}: exec must equal the id (the Makefile stages the executable under the id)")
        if not any(n.startswith("LICENSE.") for n in os.listdir(os.path.join(root, folder))):
            raise BuildError(f"{folder}: no LICENSE.* file for the core")
        for s in m["systems"]:
            other = games.setdefault(s["games_dir"].lower(), folder)
            if other != folder:
                print(f"note: games folder {s['games_dir']} is shared by {other} and {folder}")
    print(f"{root}: {len(folders)} manifest(s) valid")


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
    p.add_argument("--check-manifests", metavar="DIR", help="only check the emulator.json files under DIR")
    args = p.parse_args()
    try:
        if args.check_manifests:
            check_manifests(args.check_manifests)
        elif args.validate:
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
