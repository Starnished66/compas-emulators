# Emulator protocol 1

This is the interface between the Compás player ([compas-player](https://github.com/Starnished66/compas-player))
and the emulator packages in this repository. Both sides implement exactly this. `tools/build_index.py` enforces the
manifest and catalog rules at build time.

## Installed layout (on the device)

Root: `.emulators` at the root of the SD card (`/data/mnt/sd_0/.emulators` on the device), next to the
player's `.plugins`. Host builds of the player use `./music/.emulators`. While the SD card is not mounted
(or the player is in USB mass storage mode) no emulator is installed or playable. The card is vfat, which
keeps no Unix modes: every file reads as executable, so installers treat a failing chmod as harmless.
One folder per emulator, named after its id:

```
<root>/<id>/emulator.json   manifest (below)
<root>/<id>/<exec>          static mipsel executable, mode 0755
<root>/<id>/LICENSE*        license texts (optional, any number)
```

`<id>` matches `^[a-z0-9][a-z0-9_-]{0,31}$`. A folder whose name differs from the manifest id, whose manifest
does not parse, whose `protocol` is not 1, or whose exec file is missing or not executable, is ignored by the
player (log one line, show nothing).

## Manifest `emulator.json`

```json
{
  "id": "quicknes",
  "name": "QuickNES",
  "version": "1.0.0",
  "protocol": 1,
  "exec": "quicknes",
  "systems": [
    { "id": "nes", "name": "NES", "games_dir": "NES", "extensions": ["nes"] }
  ],
  "author": "Compás",
  "license": "LGPL-2.1-or-later",
  "description": "Nintendo Entertainment System emulator based on QuickNES."
}
```

- `name` <= 64 bytes, `version` <= 31, `exec` a plain file name (no `/`, <= 63), `description` <= 400.
- `systems`: 1 to 8 entries. Each has `id` (letters, digits, `_`, `-`, <= 15), `name` (<= 32), `games_dir`
  (a plain folder name: no `/`, not `.`/`..`, <= 63) and `extensions` (lowercase, without the dot, 1 to 8 per
  system, each <= 7 bytes, letters and digits).
- `free_player_memory` (optional, boolean, default false): the player releases its rebuildable caches
  (screen snapshots, decoded images, thumbnails) before launching this emulator, for emulators that need most of
  the device memory, such as Doom. Light emulators leave it off so returning to the player stays instant.
- Unknown keys are ignored (forward compatibility). The emulators in this repository keep their frontend settings
  under a `frontend` key, see `frontend/config.h` and README.md.

The limits above are what the player enforces; the manifest file itself must be <= 16 KiB.

## Games

Games for a system live in `<SD>/Games/<games_dir>/` (target SD root `/data/mnt/sd_0`, host `./music`).
Flat folder; files whose extension matches (case-insensitive) are games; names starting with `.` are
skipped; the label is the file name without extension, sorted case-insensitively. Save files the emulator
writes next to the ROM (for example `<rom>.srm`) are not games because their extensions do not match.

An emulator package may also bundle games: files installed under `<root>/<id>/games/<games_dir>/` (same
`games_dir` names and extension rules) are listed together with the SD card's games of that system. The packager
(`tools/build_index.py`) accepts nested files only under `games/<games_dir>/` of a system the manifest declares.

## Launch (protocol 1)

The player:
1. Refuses with a toast when Wi-Fi DAC, Bluetooth DAC or USB DAC mode is active.
2. Pauses playback if it is playing (does not resume it afterwards).
3. `posix_spawn`s `<root>/<id>/<exec>` with argv `{ "<root>/<id>/<exec>", "<absolute rom path>", NULL }`,
   working directory `<root>/<id>`, stdin `/dev/null`, stdout and stderr written to
   `/tmp/compas_emulator.log` (truncated at each launch), every other inherited fd closed, and environment
   `COMPAS_BOARD=<r1|r3proii|r3ii_2025>` and `COMPAS_EMULATOR_PROTOCOL=1` added to the player's environment.
4. Blocks its UI thread in `waitpid` (intended: nothing draws, screen timeout and idle suspend cannot fire),
   then calls `gui_resume_after_external_session()` (resets inactivity, discards the blocked time from the LVGL
   clock, flushes queued button events, full repaint).
5. Exit status 0 is a normal quit. A non-zero status or a signal shows "The game closed unexpectedly.".
   A spawn failure shows "Could not start the game.".

The emulator, while it runs:
- Owns `/dev/fb0` (RGB565, double buffered by panning) and restores the original pan offset before exiting.
- `EVIOCGRAB`s every `/dev/input/event*` so the player sees no input.
- Opens ALSA card 0 device 0 and retries for up to 2 s, because the player closes its device asynchronously
  after a pause; runs silent if it still cannot.
- Quits from its pause menu (EXIT) or on a 1 s hold of KEY_POWER; before exiting waits (max 2 s) until no
  key and no touch is down, so release events do not reach the player.
- Exits by itself (status 3) if its main loop stalls for 5 s.
- Writes saves next to the ROM.

## Catalog (store)

Published as a release asset `index.json` at
`https://github.com/Starnished66/compas-emulators/releases/latest/download/index.json`, with each package
file as another asset of the same release. Same shape as the compas-plugins catalog, with an `emulators`
array instead of `plugins`, `protocol` instead of `api_min`, and an `executable` flag per file:

```json
{
  "schema": 1,
  "tag": "v2026.10.16.1",
  "emulators": [
    {
      "id": "quicknes",
      "name": "QuickNES",
      "version": "1.0.0",
      "protocol": 1,
      "systems": ["NES"],
      "description": "Nintendo Entertainment System emulator based on QuickNES.",
      "author": "Compás",
      "license": "LGPL-2.1-or-later",
      "size": 1234567,
      "files": [
        { "asset": "quicknes--quicknes", "dest": "quicknes/quicknes", "sha256": "...", "size": 1200000, "executable": true },
        { "asset": "quicknes--emulator.json", "dest": "quicknes/emulator.json", "sha256": "...", "size": 400 },
        { "asset": "quicknes--LICENSE", "dest": "quicknes/LICENSE", "sha256": "...", "size": 26000 }
      ]
    }
  ]
}
```

The player limits the catalog to 64 KiB, 32 emulators, 16 files per emulator and 64 MiB per file, each emulator
needs `<id>/emulator.json` among its files and at least one `executable` file, and `size` must equal the sum of
its file sizes. `dest` is relative to the emulators root and must start with `<id>/`; no `..`, no absolute paths. The player
verifies size and sha256 of every file before installing, installs atomically per emulator (download into a
temporary folder, then rename over the old one), sets mode 0755 on `executable` files and 0644 otherwise.
Entries with `protocol` != 1 are shown as incompatible and cannot be installed.
