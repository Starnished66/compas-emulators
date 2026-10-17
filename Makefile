# Compas emulator packages. See README.md and docs/ADDING_AN_EMULATOR.md.
#
#   make             device executables: build/target/<id>/<id>
#   make selftest    host build of the frontend self-test (NES, QuickNES) plus the
#                    manifest checks of every emulator
#   make smoke       runs every device executable under qemu-mipsel on a test ROM
#                    (tools/smoke.sh; skipped when qemu-mipsel or the ROMs are missing)
#   make package     stage each emulator and build dist/ and dist/index.json
#
# Each emulator is emulators/<id>/core.mk (core source rules) and
# emulators/<id>/emulator.json; add its id to EMULATORS.

EMULATORS = quicknes gpsp gearsystem gearboy wswan pce prboom

# Built but not shipped: HELD emulators (emulators/<id>) wait on a decision
# and are staged with `make stage-<id>`; moving the id to EMULATORS ships it.
# EXPERIMENTAL cores (experimental/<id>, performance tests only, never packaged)
# are built with `make experimental`.
HELD =
EXPERIMENTAL = clownmd blastem

CROSS_PREFIX ?= mipsel-linux-musl-
CROSS_CC = $(CROSS_PREFIX)gcc
CROSS_CXX = $(CROSS_PREFIX)g++
CROSS_STRIP = $(CROSS_PREFIX)strip

TAG ?= dev
PYTHON ?= python3

# tinyalsa pinned to the commit compas-player ships, the one this frontend's
# audio path was proven with on a real R1 (v2.0.0, the latest tag, is 2021).
TINYALSA_DIR = third_party/tinyalsa
TINYALSA_URL = https://github.com/tinyalsa/tinyalsa.git
TINYALSA_COMMIT = 9fab97ca07184371ecad81154d1dadb09d0fa7cf
# Only what PCM output needs; the mixer is not used.
TINYALSA_SRCS = limits.c pcm.c pcm_hw.c pcm_plugin.c snd_card_plugin.c

TARGET_CPU_FLAGS = -march=mips32r2 -mabi=32 -mfp32 -mno-odd-spreg -ffp-contract=off
TARGET_SECTION_FLAGS = -ffunction-sections -fdata-sections
TARGET_LDFLAGS = -static -no-pie -Wl,--gc-sections -lpthread -lm

FRONTEND_SRCS = frontend/frontend.c frontend/config.c frontend/stretch.c frontend/present.c frontend/xrun.c frontend/glyph5x7.c
FRONTEND_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -MMD -MP -Ifrontend
TINYALSA_CFLAGS = -std=gnu11 -O2 -Wall -Wno-stringop-truncation -MMD -MP -I$(TINYALSA_DIR)/include -I$(TINYALSA_DIR)/src

BUILD = build
HOST_DIR = $(BUILD)/host
TARGET_DIR = $(BUILD)/target
STAGE_DIR = $(BUILD)/stage
DIST = dist

.DEFAULT_GOAL := all
.PHONY: all selftest smoke package experimental clean distclean

# Replaces $(1) with a checkout of commit $(3) of repository $(2).
define git_fetch
	@echo "Fetching $(2) $(3)"
	rm -rf $(1)
	git init -q $(1)
	git -C $(1) fetch -q --depth 1 $(2) $(3)
	git -C $(1) checkout -q FETCH_HEAD
endef

$(TINYALSA_DIR)/.pinned:
	$(call git_fetch,$(TINYALSA_DIR),$(TINYALSA_URL),$(TINYALSA_COMMIT))
	touch $@

# One block per emulator, given its id. Expects from emulators/<id>/core.mk:
#   <id>_SRCS      core sources (.c, .cc, .cpp, .S) under third_party/<id>/
#   <id>_CFLAGS    flags for C and assembler sources
#   <id>_CXXFLAGS  flags for C++ sources
#   <id>_INC       include path holding libretro.h, for the frontend
#   <id>_READY     a file that exists once the core is fetched
# and optionally <id>_SELFTEST = 1 when the host self-test (NES ROM) applies and
# <id>_EXTRA = files packaged with the executable, as source=path-inside-the-package
# entries (a path under games/<games_dir>/ is a game bundled with the emulator).
define emulator_rules
$(1)_HOST_CORE = $$($(1)_SRCS:third_party/$(1)/%=$(HOST_DIR)/$(1)/core/%.o)
$(1)_TARGET_CORE = $$($(1)_SRCS:third_party/$(1)/%=$(TARGET_DIR)/$(1)/core/%.o)
$(1)_HOST_FRONT = $$(FRONTEND_SRCS:%.c=$(HOST_DIR)/$(1)/%.o)
$(1)_TARGET_FRONT = $$(FRONTEND_SRCS:%.c=$(TARGET_DIR)/$(1)/%.o) $(TARGET_DIR)/$(1)/frontend/main.o

$(HOST_DIR)/$(1)/core/%.c.o: third_party/$(1)/%.c | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_CFLAGS) -MMD -MP -c $$< -o $$@

$(HOST_DIR)/$(1)/core/%.cc.o: third_party/$(1)/%.cc | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$($(1)_CXXFLAGS) -MMD -MP -c $$< -o $$@

$(HOST_DIR)/$(1)/core/%.cpp.o: third_party/$(1)/%.cpp | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$($(1)_CXXFLAGS) -MMD -MP -c $$< -o $$@

$(TARGET_DIR)/$(1)/core/%.c.o: third_party/$(1)/%.c | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CROSS_CC) $$($(1)_CFLAGS) $$(TARGET_CPU_FLAGS) $$(TARGET_SECTION_FLAGS) -MMD -MP -c $$< -o $$@

$(TARGET_DIR)/$(1)/core/%.S.o: third_party/$(1)/%.S | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CROSS_CC) $$($(1)_CFLAGS) $$(TARGET_CPU_FLAGS) $$(TARGET_SECTION_FLAGS) -MMD -MP -c $$< -o $$@

$(TARGET_DIR)/$(1)/core/%.cc.o: third_party/$(1)/%.cc | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CROSS_CXX) $$($(1)_CXXFLAGS) $$(TARGET_CPU_FLAGS) $$(TARGET_SECTION_FLAGS) -MMD -MP -c $$< -o $$@

$(TARGET_DIR)/$(1)/core/%.cpp.o: third_party/$(1)/%.cpp | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CROSS_CXX) $$($(1)_CXXFLAGS) $$(TARGET_CPU_FLAGS) $$(TARGET_SECTION_FLAGS) -MMD -MP -c $$< -o $$@

$(HOST_DIR)/$(1)/frontend/%.o: frontend/%.c | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(FRONTEND_CFLAGS) -DHOST_BUILD=1 -DEMULATOR_ID='"$(1)"' $$($(1)_INC) -c $$< -o $$@

$(TARGET_DIR)/$(1)/frontend/%.o: frontend/%.c $(TINYALSA_DIR)/.pinned | $$($(1)_READY)
	@mkdir -p $$(dir $$@)
	$$(CROSS_CC) $$(FRONTEND_CFLAGS) -DEMULATOR_ID='"$(1)"' $$($(1)_INC) -I$(TINYALSA_DIR)/include \
	    $$(TARGET_CPU_FLAGS) $$(TARGET_SECTION_FLAGS) -c $$< -o $$@

$(HOST_DIR)/$(1)/selftest: frontend/selftest.c $$($(1)_HOST_FRONT) $$($(1)_HOST_CORE)
	$$(CC) $$(filter-out -MMD -MP,$$(FRONTEND_CFLAGS)) -DHOST_BUILD=1 -DEMULATOR_ID='"$(1)"' $$($(1)_INC) $$< $$($(1)_HOST_FRONT) $$($(1)_HOST_CORE) \
	    -lm -lpthread -lstdc++ -o $$@

$(TARGET_DIR)/$(1)/$(1)_unstripped: $$($(1)_TARGET_FRONT) $$($(1)_TARGET_CORE) $(TARGET_DIR)/tinyalsa.a
	$$(CROSS_CXX) $$(TARGET_CPU_FLAGS) -o $$@ $$($(1)_TARGET_FRONT) $$($(1)_TARGET_CORE) \
	    $(TARGET_DIR)/tinyalsa.a $$(TARGET_LDFLAGS)

$(TARGET_DIR)/$(1)/$(1): $(TARGET_DIR)/$(1)/$(1)_unstripped
	$$(CROSS_STRIP) -s -o $$@ $$<

.PHONY: selftest-$(1) stage-$(1)
selftest-$(1): $(HOST_DIR)/$(1)/selftest
	./$(HOST_DIR)/$(1)/selftest

stage-$(1): $(TARGET_DIR)/$(1)/$(1) $$(foreach f,$$($(1)_EXTRA),$$(firstword $$(subst =, ,$$(f))))
	rm -rf $(STAGE_DIR)/$(1)
	mkdir -p $(STAGE_DIR)/$(1)
	cp $(TARGET_DIR)/$(1)/$(1) $(STAGE_DIR)/$(1)/$(1)
	cp emulators/$(1)/emulator.json $(STAGE_DIR)/$(1)/
	cp LICENSE $(STAGE_DIR)/$(1)/LICENSE
	cp emulators/$(1)/LICENSE.* $(STAGE_DIR)/$(1)/
	cp $(TINYALSA_DIR)/NOTICE $(STAGE_DIR)/$(1)/LICENSE.tinyalsa
	$$(foreach f,$$($(1)_EXTRA),mkdir -p $(STAGE_DIR)/$(1)/$$(dir $$(lastword $$(subst =, ,$$(f)))) && \
	    cp $$(firstword $$(subst =, ,$$(f))) $(STAGE_DIR)/$(1)/$$(lastword $$(subst =, ,$$(f))) &&) true

$$(if $$(filter $(1),$(EXPERIMENTAL)),experimental,all): $(TARGET_DIR)/$(1)/$(1)
$$(if $$(filter $(1),$(EMULATORS)),$$(if $$($(1)_SELFTEST),selftest: selftest-$(1)))
$$(if $$(filter $(1),$(EMULATORS)),package: stage-$(1))

-include $$($(1)_HOST_CORE:.o=.d) $$($(1)_TARGET_CORE:.o=.d) $$($(1)_HOST_FRONT:.o=.d) $$($(1)_TARGET_FRONT:.o=.d)
endef

include $(foreach id,$(EMULATORS) $(HELD),emulators/$(id)/core.mk) $(foreach id,$(EXPERIMENTAL),experimental/$(id)/core.mk)
$(foreach id,$(EMULATORS) $(HELD) $(EXPERIMENTAL),$(eval $(call emulator_rules,$(id))))

# tinyalsa PCM output, shared by every emulator.
TINYALSA_OBJS = $(TINYALSA_SRCS:%.c=$(TARGET_DIR)/tinyalsa/%.o)

# The sources appear only when the pinned fetch runs; without this rule a
# clean checkout finds no way to make them and skips the pattern rule below.
$(TINYALSA_SRCS:%=$(TINYALSA_DIR)/src/%): $(TINYALSA_DIR)/.pinned ;

$(TARGET_DIR)/tinyalsa/%.o: $(TINYALSA_DIR)/src/%.c $(TINYALSA_DIR)/.pinned
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TINYALSA_CFLAGS) $(TARGET_CPU_FLAGS) $(TARGET_SECTION_FLAGS) -c $< -o $@

$(TARGET_DIR)/tinyalsa.a: $(TINYALSA_OBJS)
	rm -f $@
	$(CROSS_PREFIX)ar rcs $@ $^

-include $(TINYALSA_OBJS:.o=.d)

# Manifests of every emulator against the player's real rules (tools/build_index.py).
selftest:
	$(PYTHON) tools/build_index.py --check-manifests emulators

# Runs each device executable under qemu-mipsel on a test ROM, when both exist.
smoke: all
	tools/smoke.sh $(EMULATORS)

# Staged packages -> dist/ (assets named <id>--<file>) and dist/index.json.
package:
	$(PYTHON) tools/build_index.py --tag "$(TAG)" --stage $(STAGE_DIR) --out $(DIST)

clean:
	rm -rf $(BUILD) $(DIST)

distclean: clean
	rm -rf third_party
