# gpSP (Game Boy Advance), libretro core by Exophase and the libretro team,
# GPL-2.0-or-later. Built with its MIPS dynamic recompiler, using the same
# flags as upstream's own "gcw0" platform in its Makefile, and its built-in
# BIOS (bios_data.S), so no BIOS file is needed.

gpsp_DIR = third_party/gpsp
gpsp_URL = https://github.com/libretro/gpsp.git
gpsp_COMMIT = 5819380c2ffb0900219d700a382ee68c464ebb99
gpsp_READY = $(gpsp_DIR)/Makefile.common

$(gpsp_READY):
	$(call git_fetch,$(gpsp_DIR),$(gpsp_URL),$(gpsp_COMMIT))

# The exact source lists from upstream's Makefile.common, for the dynarec
# build on MIPS.
CORE_DIR := $(gpsp_DIR)
HAVE_DYNAREC := 1
CPU_ARCH := mips
SOURCES_ASM :=
SOURCES_CC :=
SOURCES_C :=
include $(gpsp_READY)
gpsp_SRCS := $(SOURCES_C) $(SOURCES_CC) $(SOURCES_ASM)
HAVE_DYNAREC :=
CPU_ARCH :=

gpsp_INC = -I$(gpsp_DIR)/libretro/libretro-common/include
# Upstream's DEFINES plus the gcw0 platform flags (-DMIPS_HAS_R2_INSTS,
# -fomit-frame-pointer -ffast-math -mhard-float); no -fPIC because the core is
# linked statically. The translation cache is a writable+executable .jit
# section, as upstream does when MMAP_JIT_CACHE is off.
# -fPIC -DPIC as upstream's MIPS platforms: the recompiler then loads $$t9 before
# calling C helpers, which position-independent MIPS code needs.
gpsp_CFLAGS = -fPIC -DPIC -O3 -DNDEBUG -fomit-frame-pointer -ffast-math -mhard-float -DMIPS_HAS_R2_INSTS \
              -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -D__LIBRETRO__ -DINLINE=inline \
              -DHAVE_DYNAREC -DMIPS_ARCH -DFRONTEND_SUPPORTS_RGB565 \
              -I$(gpsp_DIR)/libretro -I$(gpsp_DIR)/libretro/libretro-common/include -I$(gpsp_DIR)/
gpsp_CXXFLAGS = $(gpsp_CFLAGS) -fno-rtti -fno-exceptions -std=c++11
