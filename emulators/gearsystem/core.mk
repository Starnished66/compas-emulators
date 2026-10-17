# Gearsystem (Master System, Game Gear, SG-1000), libretro core by Ignacio
# Sanchez, GPL-3.0-or-later. No BIOS is needed (the optional boot ROMs are off
# unless asked for).

gearsystem_DIR = third_party/gearsystem
gearsystem_URL = https://github.com/drhelius/Gearsystem.git
gearsystem_COMMIT = 190873a34c3f4d239cadf11c3ce87688bec24b40
gearsystem_READY = $(gearsystem_DIR)/platforms/libretro/Makefile.common

$(gearsystem_READY):
	$(call git_fetch,$(gearsystem_DIR),$(gearsystem_URL),$(gearsystem_COMMIT))

# The exact source lists from upstream's libretro Makefile.common.
CORE_DIR := $(gearsystem_DIR)/platforms/libretro
SOURCE_DIR := $(gearsystem_DIR)/src
DEPS_DIR := $(gearsystem_DIR)/platforms/shared/dependencies
SOURCES_C :=
SOURCES_CXX :=
include $(gearsystem_READY)
gearsystem_SRCS := $(SOURCES_C) $(SOURCES_CXX)

gearsystem_INC = -I$(gearsystem_DIR)/platforms/libretro
# Upstream's release flags and defines (-O3 -DNDEBUG -fno-rtti -fno-exceptions,
# no disassembler or VGM recorder); no -fPIC or LTO since the core is linked
# statically with the frontend.
gearsystem_CFLAGS = -O3 -DNDEBUG -DGS_DISABLE_DISASSEMBLER -DGS_DISABLE_VGMRECORDER -D__LIBRETRO__ -fno-exceptions \
                    -I$(gearsystem_DIR)/platforms/libretro -I$(gearsystem_DIR)/src -I$(gearsystem_DIR)/platforms/shared/dependencies/miniz -w -std=gnu99
gearsystem_CXXFLAGS = -O3 -DNDEBUG -DGS_DISABLE_DISASSEMBLER -DGS_DISABLE_VGMRECORDER -D__LIBRETRO__ -fno-exceptions -fno-rtti \
                      -I$(gearsystem_DIR)/platforms/libretro -I$(gearsystem_DIR)/src -I$(gearsystem_DIR)/platforms/shared/dependencies/miniz -w -std=c++11
