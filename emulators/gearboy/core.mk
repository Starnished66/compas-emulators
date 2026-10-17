# Gearboy (Game Boy, Game Boy Color), libretro core by Ignacio Sanchez,
# GPL-3.0-or-later. No BIOS is needed (the optional boot ROMs are off unless
# asked for).

gearboy_DIR = third_party/gearboy
gearboy_URL = https://github.com/drhelius/Gearboy.git
gearboy_COMMIT = 4f81bba68d98a275a794771279ed863c83e04e14
gearboy_READY = $(gearboy_DIR)/platforms/libretro/Makefile.common

$(gearboy_READY):
	$(call git_fetch,$(gearboy_DIR),$(gearboy_URL),$(gearboy_COMMIT))

# The exact source lists from upstream's libretro Makefile.common.
CORE_DIR := $(gearboy_DIR)/platforms/libretro
SOURCE_DIR := $(gearboy_DIR)/src
DEPS_DIR := $(gearboy_DIR)/platforms/shared/dependencies
SOURCES_C :=
SOURCES_CXX :=
include $(gearboy_READY)
gearboy_SRCS := $(SOURCES_C) $(SOURCES_CXX)

gearboy_INC = -I$(gearboy_DIR)/platforms/libretro
# Upstream's release flags and defines; no -fPIC or LTO since the core is
# linked statically with the frontend.
gearboy_CFLAGS = -O3 -DNDEBUG -DGEARBOY_DISABLE_DISASSEMBLER -DGEARBOY_DISABLE_VGMRECORDER -D__LIBRETRO__ -fno-exceptions \
                 -I$(gearboy_DIR)/platforms/libretro -I$(gearboy_DIR)/src -I$(gearboy_DIR)/platforms/shared/dependencies/miniz -w -std=gnu99
gearboy_CXXFLAGS = -O3 -DNDEBUG -DGEARBOY_DISABLE_DISASSEMBLER -DGEARBOY_DISABLE_VGMRECORDER -D__LIBRETRO__ -fno-exceptions -fno-rtti \
                   -I$(gearboy_DIR)/platforms/libretro -I$(gearboy_DIR)/src -I$(gearboy_DIR)/platforms/shared/dependencies/miniz -w -std=c++11
