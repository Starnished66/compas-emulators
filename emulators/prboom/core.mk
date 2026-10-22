# PrBoom (Doom engine), libretro core with its own libretro-common, GPL-2.0-or-later
# (see LICENSE.PrBoom for the per-file notices). The engine data file prboom.wad
# is committed in the upstream repository and is packaged next to the
# executable (the emulator folder is the core's system directory). Freedoom
# Phase 1 is bundled as the free game; other WADs go in Games/Doom.

prboom_DIR = third_party/prboom
prboom_URL = https://github.com/libretro/libretro-prboom.git
prboom_COMMIT = c6e0fcb8325fc7969c91387f56c63433de0f1a53
prboom_UPSTREAM_READY = $(prboom_DIR)/Makefile.common
prboom_PATCH = $(abspath emulators/prboom/patches/fix-composite-cache-owner.patch)
prboom_READY = $(prboom_DIR)/.compas-composite-cache-owner
prboom_EXTRA = $(prboom_DIR)/prboom.wad

$(prboom_UPSTREAM_READY):
	$(call git_fetch,$(prboom_DIR),$(prboom_URL),$(prboom_COMMIT))

# Keep cache owner pointers valid when the low-memory zone purges composites.
$(prboom_READY): $(prboom_UPSTREAM_READY) $(prboom_PATCH)
	@if git -C $(prboom_DIR) apply --check $(prboom_PATCH); then \
		git -C $(prboom_DIR) apply $(prboom_PATCH); \
	elif git -C $(prboom_DIR) apply --reverse --check $(prboom_PATCH); then \
		:; \
	else \
		echo "PrBoom composite cache owner patch does not match the source" >&2; exit 1; \
	fi
	touch $@

# The exact source list from upstream's Makefile.common, for its "unix"
# platform (threaded renderer on, no FluidSynth).
ROOT_DIR := $(prboom_DIR)
CORE_DIR := $(prboom_DIR)/src
platform := unix
HAVE_THREADS :=
WANT_FLUIDSYNTH := 0
STATIC_LINKING := 0
SOURCES_C :=
include $(prboom_UPSTREAM_READY)
prboom_SRCS := $(SOURCES_C)
prboom_DEFS := $(COREDEFINES)
# Normal dependencies rebuild existing objects when the tracked patch changes.
$(prboom_SRCS:third_party/prboom/%=$(HOST_DIR)/prboom/core/%.o) $(prboom_SRCS:third_party/prboom/%=$(TARGET_DIR)/prboom/core/%.o): $(prboom_READY)
platform :=
SOURCES_C :=

prboom_INC = -I$(prboom_DIR)/libretro/libretro-common/include
# Upstream's unix flags: -std=c99 with _POSIX_C_SOURCE and _DEFAULT_SOURCE,
# HAVE_MMAP, HAVE_THREADS and NDEBUG; raise optimization to -O3 for the core.
# No -fPIC (linked statically).
prboom_CFLAGS = -std=c99 -D_POSIX_C_SOURCE=199309L -D_DEFAULT_SOURCE -DHAVE_MMAP -DHAVE_THREADS -DMEMORY_LOW -O3 -DNDEBUG \
                $(prboom_DEFS) -fomit-frame-pointer -w \
                -I$(prboom_DIR) -I$(prboom_DIR)/src -I$(prboom_DIR)/libretro/libretro-common/include
prboom_CXXFLAGS = $(prboom_CFLAGS)

# Freedoom 0.13.0 (BSD-3-Clause), the free game shipped with the package as
# games/Doom/freedoom1.wad. freedoom2.wad is not shipped: its larger maps run
# out of memory on the R1 next to the player. Downloaded from the official
# release on GitHub; the archive and both WADs are checked against pinned
# SHA-256 sums (the release's own freedoom-0.13.0-CHECKSUM file lists the
# archive sum).
freedoom_DIR = third_party/freedoom
freedoom_URL = https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
freedoom_ZIP_SHA256 = 3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
freedoom1_SHA256 = 7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d

$(freedoom_DIR)/freedoom1.wad: $(freedoom_DIR)/.fetched ;

$(freedoom_DIR)/.fetched:
	rm -rf $(freedoom_DIR)
	mkdir -p $(freedoom_DIR)
	curl --silent --show-error --fail --location --retry 2 --output $(freedoom_DIR)/freedoom.zip $(freedoom_URL)
	echo "$(freedoom_ZIP_SHA256)  $(freedoom_DIR)/freedoom.zip" | sha256sum --check --status
	unzip -q -j -o $(freedoom_DIR)/freedoom.zip freedoom-0.13.0/freedoom1.wad -d $(freedoom_DIR)
	echo "$(freedoom1_SHA256)  $(freedoom_DIR)/freedoom1.wad" | sha256sum --check --status
	rm $(freedoom_DIR)/freedoom.zip
	touch $@

# Files packaged next to the executable: source=path inside the package.
prboom_EXTRA = $(prboom_DIR)/prboom.wad=prboom.wad \
               $(freedoom_DIR)/freedoom1.wad=games/Doom/freedoom1.wad
$(prboom_DIR)/prboom.wad: $(prboom_READY) ;
