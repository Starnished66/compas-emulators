# QuickNES (NES), libretro core by blargg and kode54, LGPL-2.1-or-later.
# Included by the top Makefile for every id in EMULATORS. Variables are
# prefixed with the emulator id; see docs/ADDING_AN_EMULATOR.md.

quicknes_DIR = third_party/quicknes
quicknes_URL = https://github.com/libretro/QuickNES_Core.git
quicknes_COMMIT = 26bb785c9deddb66a17717b21bb4e328f03ade32
quicknes_READY = $(quicknes_DIR)/Makefile.common

$(quicknes_READY):
	$(call git_fetch,$(quicknes_DIR),$(quicknes_URL),$(quicknes_COMMIT))

# The exact SOURCES_CXX list from upstream's own Makefile.common (libretro.cpp
# plus nes_emu/*.cpp), not a glob. Included for that list only: upstream's
# INCFLAGS and the libretro-common sources are not used.
CORE_DIR := $(quicknes_DIR)
include $(quicknes_READY)
quicknes_SRCS := $(SOURCES_CXX)

# libretro.h comes from the core's own libretro-common copy.
quicknes_INC = -I$(quicknes_DIR)/libretro/libretro-common/include
# HAVE_NO_LANGEXTRA drops about 300 KB of translated core-option text the
# frontend never uses.
quicknes_SELFTEST = 1
quicknes_CXXFLAGS = -O2 -DNDEBUG -D__LIBRETRO__ -DHAVE_NO_LANGEXTRA -Wno-multichar -Wno-sign-compare -Wno-deprecated-enum-enum-conversion \
                    -I$(quicknes_DIR) -I$(quicknes_DIR)/nes_emu -I$(quicknes_DIR)/libretro $(quicknes_INC)
