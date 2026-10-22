# Beetle PCE Fast (PC Engine / TurboGrafx-16 HuCards), the Mednafen PCE Fast
# core ported to libretro, GPL-2.0-or-later (every compiled source file carries
# the "or any later version" notice). HuCard games need no BIOS; the CD-ROM
# code is compiled as upstream does but CD games need a System Card, so the
# manifest lists cartridge files only. CHD support is left out.

pce_DIR = third_party/pce
pce_URL = https://github.com/libretro/beetle-pce-fast-libretro.git
pce_COMMIT = 3f946f277aef3aa99a95551618bbcd1dd2bda0d9
pce_READY = $(pce_DIR)/Makefile.common

$(pce_READY):
	$(call git_fetch,$(pce_DIR),$(pce_URL),$(pce_COMMIT))

# The exact source lists from upstream's Makefile.common, with the options
# its Makefile sets (Tremor and Blip_Buffer on, RGB565 output), minus CHD.
CORE_DIR := $(pce_DIR)
NEED_TREMOR := 1
NEED_BLIP := 1
NEED_CD := 1
FRONTEND_SUPPORTS_RGB565 := 1
HAVE_CHD :=
HAVE_CDROM :=
STATIC_LINKING :=
FLAGS :=
SOURCES_C :=
SOURCES_CXX :=
include $(pce_READY)
pce_SRCS := $(SOURCES_C) $(SOURCES_CXX)
pce_FLAGS := $(FLAGS)
NEED_TREMOR :=
NEED_BLIP :=
NEED_CD :=
FRONTEND_SUPPORTS_RGB565 :=

pce_INC = -I$(pce_DIR)/libretro-common/include
# Upstream defines and loop policy, with -O3 for this emulator's hot code.
pce_CFLAGS = -O3 -DNDEBUG -funroll-loops $(pce_FLAGS) -DMEDNAFEN_VERSION_NUMERIC=931 -DWANT_PCE_FAST_EMU \
             -DSTDC_HEADERS -D__STDC_LIMIT_MACROS -D__LIBRETRO__ -D_LOW_ACCURACY_ -DINLINE=inline -w -std=gnu99 \
             -I$(pce_DIR) -I$(pce_DIR)/mednafen -I$(pce_DIR)/mednafen/include -I$(pce_DIR)/mednafen/hw_sound \
             -I$(pce_DIR)/mednafen/hw_cpu -I$(pce_DIR)/mednafen/hw_misc -I$(pce_DIR)/libretro-common/include
pce_CXXFLAGS = $(pce_CFLAGS)
