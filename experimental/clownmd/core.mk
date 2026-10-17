# EXPERIMENTAL, not shipped: ClownMDEmu (Sega Mega Drive / Genesis), AGPL-3.0
# (LICENCE.txt of the repository and of the common/core, clown68000 and clownz80
# submodules; clowncommon, clownresampler and clowncd are 0BSD-style permissive).
# Only a performance test; see README.md. Built as upstream's unity build
# (Makefile.common: unity.c), without CHD disc support (cartridge games only).
# Submodules are fetched at the commits the pinned parent records.

clownmd_DIR = third_party/clownmd
clownmd_URL = https://github.com/Clownacy/clownmdemu-libretro.git
clownmd_COMMIT = d43c2708b0a31c285ce16724b6c4a2e92af07346
clownmd_READY = $(clownmd_DIR)/unity.c

$(clownmd_READY):
	$(call git_fetch,$(clownmd_DIR),$(clownmd_URL),$(clownmd_COMMIT))
	git -C $(clownmd_DIR) submodule update --init --recursive --depth 1 -q

clownmd_SRCS = $(clownmd_DIR)/unity.c
clownmd_INC = -I$(clownmd_DIR)/libretro-common/include
# -DCC_RESAMPLER_NO_HIGHPASS and -ffast-math as upstream's MIPS (rs90) platform.
clownmd_CFLAGS = -std=gnu11 -O2 -DNDEBUG -DCLOWNCD_NO_CHD -DCC_RESAMPLER -DCC_RESAMPLER_NO_HIGHPASS -fomit-frame-pointer \
                 -w -I$(clownmd_DIR) -I$(clownmd_DIR)/libretro-common/include
clownmd_CXXFLAGS = $(clownmd_CFLAGS)
# Non-PIC static code (no abicalls, no GOT calls): the fastest flag set measured on the R1 (tools/clownmd_variants.sh,
# "nopic"). Applies to everything linked into the clownmd executable; a command line TARGET_CPU_FLAGS still wins.
$(TARGET_DIR)/clownmd/%: TARGET_CPU_FLAGS := $(TARGET_CPU_FLAGS) -fno-pic -mno-abicalls -mno-shared
