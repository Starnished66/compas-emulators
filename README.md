# Compás Emulators

Emulator packages for [Compás](https://github.com/Starnished66/compas-player), the music player firmware for HiBy
R1, R3 Pro II and R3II 2025. The player lists them in its store, installs them in the `.emulators` folder of the SD card and
launches games from the SD card. The interface between the two is [docs/PROTOCOL.md](docs/PROTOCOL.md).

| Emulator | System | Games folder | Core |
| --- | --- | --- | --- |
| QuickNES | NES | `Games/NES`, `.nes` files | [QuickNES_Core](https://github.com/libretro/QuickNES_Core) (libretro) |

## Layout

```
frontend/            shared libretro frontend, one copy used by every emulator
emulators/<id>/      emulator.json, core.mk (how to fetch and build the core), license text
tools/build_index.py builds dist/ (assets named <id>--<file>) and dist/index.json
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
| `make selftest` | builds the frontend and each core for the host and runs the self-test (needs only a host compiler) |
| `make package TAG=v...` | stages each emulator and writes `dist/` with `index.json`; `TAG` defaults to `dev` |

Cores and tinyalsa are fetched into `third_party/` (gitignored), pinned by commit: QuickNES at
`26bb785c9deddb66a17717b21bb4e328f03ade32`, tinyalsa at the commit of release `v2.0.0`. `make distclean` removes them.

`tools/build_index.py --validate dist` re-checks a finished `dist/` against the protocol, including every file's size
and SHA-256.

## Releasing

Push a tag `v*` (for example `v2026.10.16.1`). The Release workflow builds, runs the self-test, validates the index and
creates a GitHub release with every file in `dist/` as an asset. The player reads
`https://github.com/Starnished66/compas-emulators/releases/latest/download/index.json`.

## Installing by hand

Copy the files of an emulator from a release into `.emulators/<id>/` on the SD card, and put
games in `Games/<games folder>` on the SD card.

## Adding an emulator

See [docs/ADDING_AN_EMULATOR.md](docs/ADDING_AN_EMULATOR.md).

## License

The frontend is derived from Compás player code and is GPL-3.0 ([LICENSE](LICENSE)). QuickNES is LGPL-2.1-or-later and
tinyalsa is BSD-style; each package ships those notices next to its executable.
