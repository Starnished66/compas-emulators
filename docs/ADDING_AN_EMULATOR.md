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
   - set `<id>_SRCS` to the core's C++ source list, with paths under `third_party/<id>/` (take the list from the
     core's own makefile; do not glob);
   - set `<id>_CXXFLAGS` (core defines and include paths) and `<id>_INC` (the include path holding `libretro.h`).
5. Add the id to `EMULATORS` in the `Makefile`.
6. Run `make selftest` and `make`, then try the executable on a device. The built-in self-test (`frontend/selftest.c`)
   currently generates an NES ROM; for another system, extend it or add a core-specific check.

A core written in C only still links through the C++ driver; add its sources as `.cpp` or extend the object rules
in the `Makefile` with a C pattern. The frontend targets the libretro API features QuickNES uses (video refresh in
0RGB1555, RGB565 or XRGB8888, batched or single-sample audio, joypad input on port 0, battery RAM saves); a core
that needs more (disk control, hardware rendering, other input devices) needs frontend work first.

Changing anything under `frontend/` affects every emulator. Release by pushing a tag `v*`; the player's catalog
always reads the latest release.
