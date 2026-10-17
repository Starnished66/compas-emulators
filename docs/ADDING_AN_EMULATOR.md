# Adding an emulator

Any libretro core that is plain C or C++ and can be linked statically can be added. The shared frontend in
`frontend/` provides video, touch and key input, audio, saves and the quit controls; a new emulator only supplies
the core.

1. Create `emulators/<id>/` (id matches `^[a-z0-9][a-z0-9_-]{0,31}$`).
2. Add `emulators/<id>/emulator.json`, following [PROTOCOL.md](PROTOCOL.md). `id` must equal the folder name and
   `exec` must equal the id (the Makefile stages the executable under the id).
3. Add the license text of the core as `emulators/<id>/LICENSE.<Name>`. Every `LICENSE.*` file in the folder is
   shipped with the package, together with the repository `LICENSE` and tinyalsa's notice.
4. Add `emulators/<id>/core.mk`, modelled on `emulators/quicknes/core.mk`. It must:
   - fetch the core into `third_party/<id>` pinned by commit, using `$(call git_fetch,dir,url,commit)` as the rule
     for a file that exists after the fetch (set `<id>_READY` to that file);
   - set `<id>_SRCS` to the core's source list (`.c`, `.cc`, `.cpp` and `.S` are supported), with paths under
     `third_party/<id>/`. Take the list from the core's own `Makefile.common` by `include`-ing it after the fetch
     (set the variables it expects, such as `CORE_DIR`, and clear `SOURCES_C`/`SOURCES_CXX` first), not by globbing;
   - set `<id>_CFLAGS` and `<id>_CXXFLAGS` (core defines and include paths) and `<id>_INC` (the include path
     holding `libretro.h`). Copy the defines and optimisation flags of the core's own Linux or MIPS platform in its
     Makefile; leave out `-fPIC` and LTO, the core is linked statically. Do not patch the core.
5. Add the id to `EMULATORS` in the `Makefile`. Extra files (engine data, bundled games) are listed in
   `<id>_EXTRA` as `source=path-in-the-package`; bundled games go under `games/<games_dir>/`, fetched with a checksum
   (see `emulators/prboom/core.mk`). A package holds at most 16 files of at most 64 MiB each.
6. Add the `frontend` object to `emulator.json` if the system needs other buttons than the NES set (README.md), and
   core option defaults the core would not choose itself (`options`).
7. Add a test ROM to `tools/smoke.sh` (a freely licensed one downloaded in `tools/fetch_test_roms.sh`, or a tiny
   original one from `tools/make_test_roms.py`), then run `make`, `make smoke` and `make selftest`, and try the
   executable on a device. `make selftest` (`frontend/selftest.c`) is NES based and checks the frontend itself; the
   smoke test runs `--bench` under qemu and reports a save state round trip for each core.

Things learned the hard way:

- Every `core.mk` is included into the same Makefile: the shared names a core's `Makefile.common` wants (`CORE_DIR`,
  `SOURCE_DIR`, `SOURCES_C`...) are overwritten by the next core. Use `:=` for lists taken from it, and build the
  flags from `<id>_DIR`, never from those shared names (a lazy `=` flag would see the last core's value).
- `tools/build_index.py --check-manifests emulators` applies the limits of the player's own parser, which are
  stricter than PROTOCOL.md in places (system id <= 15, system name <= 32, catalog <= 64 KiB). Run `make selftest`.
- Check the license of every compiled source file, not only the repository: some cores bundle a CPU or sound chip
  emulator under different terms. Ship notices for them in the `LICENSE.*` file.
- A core with a MIPS dynamic recompiler (gpSP) must be built with the `-fPIC -DPIC` its MIPS platforms use: the
  recompiled code then loads `$t9` before calling C helpers, which PIC MIPS code needs. Its translation cache is a
  writable and executable `.jit` segment of the executable.
- A slow core can set `frontend.frameskip` (README.md); a core that honors `RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE`
  then skips drawing on skipped frames, any other can list `skip_options` that make its rendering cheaper.
- A core that may run below real time can set `frontend.audio_stretch` (README.md): the audio is then stretched
  only while the device buffer runs low.
- Cores that render audio at a rate the codec lacks (gpSP: 32768 Hz) are resampled to 48000 Hz by the frontend.
- Cores load their own save state data; `--bench` serializes, runs 30 frames, restores, and compares. A core that
  fails the comparison (Gearsystem differs in a few bytes) still loads its states, but check it by hand.

A core written in C only still links through the C++ driver; add its sources as `.cpp` or extend the object rules
in the `Makefile` with a C pattern. The frontend supports the libretro API features the cores here use (video refresh in 0RGB1555, RGB565 or
XRGB8888 with any geometry and runtime geometry changes, batched or single-sample audio, joypad input on port 0,
battery RAM saves, save states, core options answered from `emulator.json`); a core that needs more (disk control,
hardware rendering, other input devices) needs frontend work first.

Changing anything under `frontend/` affects every emulator. Release by pushing a tag `v*`; the player's catalog
always reads the latest release.
