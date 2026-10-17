# Beetle WonderSwan (WonderSwan, WonderSwan Color), the Mednafen WonderSwan
# core ported to libretro. GPL-2.0-or-later for the core; the NEC V30MZ CPU
# emulator (v30mz.c) is by Bryan McPhail under terms that allow commercial and
# noncommercial use if the author is credited, which LICENSE.WonderSwan does.
# No BIOS is needed for cartridges.

wswan_DIR = third_party/wswan
wswan_URL = https://github.com/libretro/beetle-wswan-libretro.git
wswan_COMMIT = 4b01295838ea89e3f1355bbe4cb5cf98aa6108cd
wswan_READY = $(wswan_DIR)/Makefile.common

$(wswan_READY):
	$(call git_fetch,$(wswan_DIR),$(wswan_URL),$(wswan_COMMIT))

# The exact source list from upstream's Makefile.common, with the options
# its Makefile sets (16-bit video, Blip_Buffer audio, stereo).
CORE_DIR := $(wswan_DIR)
NEED_BPP := 16
NEED_BLIP := 1
NEED_STEREO_SOUND := 1
FRONTEND_SUPPORTS_RGB565 := 1
STATIC_LINKING :=
FLAGS :=
SOURCES_C :=
SOURCES_CXX :=
include $(wswan_READY)
wswan_SRCS := $(SOURCES_C)
wswan_FLAGS := $(FLAGS)
NEED_BPP :=
NEED_BLIP :=
NEED_STEREO_SOUND :=
FRONTEND_SUPPORTS_RGB565 :=

wswan_INC = -I$(wswan_DIR)/libretro-common/include
# Upstream's own FLAGS: -O2 -DNDEBUG -funroll-loops, the defines below, LSB_FIRST.
wswan_CFLAGS = -O2 -DNDEBUG -funroll-loops $(wswan_FLAGS) -DSIZEOF_DOUBLE=8 -DMEDNAFEN_VERSION_NUMERIC=931 \
               -DMPC_FIXED_POINT -DSTDC_HEADERS -D__STDC_LIMIT_MACROS -D__LIBRETRO__ -D_LOW_ACCURACY_ -DINLINE=inline -w -std=gnu99 \
               -I$(wswan_DIR) -I$(wswan_DIR)/mednafen -I$(wswan_DIR)/mednafen/include -I$(wswan_DIR)/mednafen/hw_sound \
               -I$(wswan_DIR)/mednafen/hw_cpu -I$(wswan_DIR)/mednafen/hw_misc -I$(wswan_DIR)/libretro-common/include
wswan_CXXFLAGS = $(wswan_CFLAGS)
