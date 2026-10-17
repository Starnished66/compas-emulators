# Compás Emulators

Emulator packages for [Compás](https://github.com/Starnished66/compas-player), the music player firmware for HiBy
R1, R3 Pro II and R3II 2025. The player lists them in its store, installs them in the `.emulators` folder of the SD card and
launches games from the SD card. The interface between the two is [docs/PROTOCOL.md](docs/PROTOCOL.md).

| Emulator | Systems (games folder under `Games/`) | Core | Core license |
| --- | --- | --- | --- |
| QuickNES | NES (`NES`: .nes) | [QuickNES_Core](https://github.com/libretro/QuickNES_Core) | LGPL-2.1-or-later |
| gpSP | Game Boy Advance (`GBA`: .gba .agb), dynamic recompiler, built-in BIOS | [gpsp](https://github.com/libretro/gpsp) | GPL-2.0-or-later |
| Gearsystem | Master System (`MasterSystem`: .sms), Game Gear (`GameGear`: .gg), SG-1000 (`SG-1000`: .sg) | [Gearsystem](https://github.com/drhelius/Gearsystem) | GPL-3.0 |
| Gearboy | Game Boy (`GB`: .gb .dmg), Game Boy Color (`GBC`: .gbc .cgb) | [Gearboy](https://github.com/drhelius/Gearboy) | GPL-3.0 |
| Beetle WonderSwan | WonderSwan (`WonderSwan`: .ws), WonderSwan Color (`WonderSwanColor`: .wsc) | [beetle-wswan-libretro](https://github.com/libretro/beetle-wswan-libretro) | GPL-2.0-or-later |
| PrBoom | Doom engine (`Doom`: .wad), ships with Freedoom Phase 1 (BSD-3-Clause) in the package | [libretro-prboom](https://github.com/libretro/libretro-prboom) | GPL-2.0 (most files "or later") |
| Beetle PCE Fast | PC Engine / TurboGrafx-16 HuCards (`PCEngine`: .pce) | [beetle-pce-fast-libretro](https://github.com/libretro/beetle-pce-fast-libretro) | GPL-2.0-or-later |

None needs a BIOS. Compressed archives (.zip, .7z) are not supported. Each package ships the core's license text next
to the executable.

## Controls

On screen: a D-pad on the left, the face buttons on the right, and SELECT and START at the bottom. Which buttons exist
is per emulator (the `frontend.buttons` list in its `emulator.json`): A and B (the NES layout), A and B plus L and R as
wide bars along the top edge (GBA), and A/B/X/Y as a diamond are supported. Touches on several controls at once work.

Physical keys (the same on all boards):

| Key | Default | Emulators with L/R | Emulators with X/Y and no L/R |
| --- | --- | --- | --- |
| Volume up | A | A | A |
| Volume down | B | B | B |
| Play/pause | START | START | START |
| Previous | SELECT | L | SELECT |
| Next | A | R | X |
| Power, short press | open or close the pause menu | | |
| Power, hold 1 s | exit, from anywhere | | |

A key whose button the emulator does not have does nothing. gpSP has L and R, so SELECT is touch only there.

### Doom (PrBoom)

All eight buttons are used (classic gamepad layout of the core): the D-pad moves forward and back (up, down) and turns
(left, right); B held makes left and right strafe; A uses and opens doors (and confirms in menus); X fires; Y runs;
L and R strafe left and right; SELECT shows or hides the automap; START opens the menu (B or SELECT goes back in menus).
The package carries `prboom.wad` (the engine's own data, built from the core's repository) and Freedoom 0.13.0
(`freedoom1.wad`, BSD-3-Clause) as a bundled game, installed under `games/Doom/` inside the
emulator's folder and listed next to the WADs you copy into `Games/Doom` on the SD card (your own Doom, Doom II or
other WADs work; an add-on WAD needs its game's IWAD in the same folder). `freedoom2.wad` is not bundled: its larger
maps run out of memory on the R1 next to the player. The settings in its `emulator.json` use the
core's 320x200 16-bit renderer, no threaded rendering, WADs memory-mapped, and a 16 MB asset cache to fit the
device's memory.

### Pause menu

Tap the middle of the game picture (a quick tap, under about 300 ms and 24 px of movement, in the middle third of the
game area both ways) or press the power key briefly. The game stops, the sound is silenced, and a dimmed picture shows
the rows RESUME, SAVE STATE, LOAD STATE, RESET and EXIT. Tap a row to use it; the power key closes the menu.

- SAVE STATE writes one slot, `<game name>.state`, next to the ROM, and stays in the menu showing SAVED or FAILED.
- LOAD STATE restores it and resumes; it is greyed out when there is no state file. A state of another size shows FAILED.
- Both are greyed out for a core that cannot save states.
- RESET restarts the game and resumes. EXIT leaves the emulator (battery saves are written as usual).

## Emulator settings in `emulator.json`

The player ignores unknown manifest keys, so each emulator keeps its frontend settings in a `frontend` object:

```json
"frontend": { "buttons": ["b", "a", "l", "r", "select", "start"], "options": { "gpsp_drc": "enabled" } }
```

`buttons` lists the on-screen buttons (any of a, b, x, y, l, r, select, start; the D-pad is always drawn; the default
is the NES set `b, a, select, start`). `options` are the answers the frontend gives when the core asks for its options
(`RETRO_ENVIRONMENT_GET_VARIABLE`); an option not listed gets the core's own default. The frontend reads the file from
its working directory, which the player sets to the emulator's folder.

`frameskip` (`off` by default, `auto` or `half`) drops the video of some frames while the audio of every frame is kept:
`auto` skips the frame after one whose `retro_run` took longer than the frame time (never two in a row), `half` every
second frame. The core is told through `RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE` (not answered while `frameskip` is
`off`), and `skip_options` are option answers used instead of `options` during a skipped frame, for a core that cannot
skip drawing but has options that turn parts of it off. For testing, the executable also takes `--opt key=value`,
`--skip-opt key=value` and `--frameskip MODE` over the manifest, and `--bench N` reports rendered against total frames
(`--budget-ms N` gives the `auto` policy another frame time).

`audio_stretch` (`false` by default) is for a core that cannot always keep up with real time. The frontend paces the
game on blocking audio writes, so a slow core drains the device buffer and the audio crackles. With `audio_stretch`
the frontend reads the buffer fill after every write and, only while the fill is low, lowers the resampling ratio
(down to 0.75) so the game plays slightly slower and lower instead; the ratio is exactly 1.0 and the audio path
unchanged while the core keeps up. At the end of a session (stderr) it logs the minimum and average
ratio, the share of time stretched and the average fill, and every 10 s while stretched the current ratio and fill.
Underruns are counted from the device itself (a write failing with EPIPE; the device is opened with `PCM_NORESTART`
so tinyalsa reports it, `frontend/xrun.c`), never estimated from the fill. Video is handed to a presenter thread
(`frontend/present.c`) so the framebuffer pan, which waits for the display, never stalls the core; the session log
ends with per-frame core, scaling, audio and pan timings.
`--audio-stretch on|off` overrides the manifest. The controller is `frontend/stretch.c`.

## Layout

```
frontend/            shared libretro frontend (video, touch, audio, pause menu), one copy used by every emulator
emulators/<id>/      emulator.json, core.mk (how to fetch and build the core), license text
tools/build_index.py builds dist/ (assets named <id>--<file>) and dist/index.json, checks manifests
tools/smoke.sh       runs each executable under qemu-mipsel on a test ROM
tools/fetch_test_roms.sh, tools/make_test_roms.py   test ROMs for the smoke test (tests/roms, gitignored)
docs/                PROTOCOL.md, ADDING_AN_EMULATOR.md
```

Each emulator is a single static executable: the frontend plus its core, linked together. The frontend takes the ROM
path as its argument (`<exec> <rom>`) and `--bench N` to run N frames headless.

## Building

Needs git, make, python3 and the `mipsel-linux-musl-` cross toolchain (GCC with musl, found on `PATH`; override the
prefix with `CROSS_PREFIX`). `.github/scripts/install-toolchain.sh DIR` downloads the one CI uses and checks its
SHA-256. From a checkout next to compas-player, its toolchain works:

```
PATH=../compas-player/toolchains/mipsel-linux-musl-cross/bin:$PATH make
```

| Command | Result |
| --- | --- |
| `make` | `build/target/<id>/<id>`, a static stripped MIPS32 executable per emulator |
| `make selftest` | builds the frontend and QuickNES for the host and runs the self-test (frontend layout, settings parser, save state round trip), and checks every manifest (needs only a host compiler) |
| `make smoke` | runs every executable under `qemu-mipsel` on a test ROM (`tools/fetch_test_roms.sh` fetches them); skipped if qemu is missing |
| `make package TAG=v...` | stages each emulator and writes `dist/` with `index.json`; `TAG` defaults to `dev` |

Cores and tinyalsa are fetched into `third_party/` (gitignored), pinned by commit (each `emulators/<id>/core.mk` has
its URL and SHA); no core is patched. `make distclean` removes them.

`tools/build_index.py --validate dist` re-checks a finished `dist/` against the protocol, including every file's size
and SHA-256.

## Releasing

Push a tag `v*` (for example `v2026.10.16.1`). The Release workflow builds, runs the self-test, validates the index and
creates a GitHub release with every file in `dist/` as an asset. The player reads
`https://github.com/Starnished66/compas-emulators/releases/latest/download/index.json`.

## Installing by hand

Copy the files of an emulator from a release into `.emulators/<id>/` on the SD card (keeping the `dest` paths of the
index, for example `games/Doom/freedoom1.wad`), and put games in `Games/<games folder>` on the SD card.

## Experimental cores

`make experimental` builds `experimental/<id>` (Sega Mega Drive / Genesis: ClownMDEmu and BlastEm) for speed
measurements on the device only. They are not in `EMULATORS` and are never packaged.
`tools/clownmd_variants.sh` builds ClownMDEmu speed-test variants (compiler flags, run options, frameskip) into
`build/target/clownmd-variants/` with a `VARIANTS.txt`, and `tools/clownmd_bench.sh` times them under qemu.

Emulators in `HELD` (`Makefile`) are built and can be staged with `make stage-<id>` but are not in `make package`
until moved to `EMULATORS`.

## Adding an emulator

See [docs/ADDING_AN_EMULATOR.md](docs/ADDING_AN_EMULATOR.md).

## License

The frontend is derived from Compás player code and is GPL-3.0 ([LICENSE](LICENSE)). The cores keep their own licenses
(table above) and tinyalsa is BSD-style; each package ships those notices next to its executable. The WonderSwan core
contains an NEC V30MZ emulator that asks for credit to its author, given in `LICENSE.WonderSwan`.
