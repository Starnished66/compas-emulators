# EXPERIMENTAL, not shipped: BlastEm (Sega Mega Drive / Genesis), GPL-3.0
# ("version 3 or greater" in the headers of the emulation files; the libretro
# glue files libblastem.c, lib_stubs.c, vfs_file.c carry no header). Only a
# performance test; see README.md. On a CPU other than x86 upstream builds its
# "new core": the 68000 and Z80 interpreters generated from m68k.cpu and z80.cpu
# by cpu_dsl.py (Python 3 needed at build time), so there is no dynamic
# recompiler in this binary. Source list = upstream's LIBOBJS for CPU=mips.

blastem_DIR = third_party/blastem
blastem_URL = https://github.com/libretro/blastem.git
blastem_COMMIT = ab743653ed110eeb7b1fe64d00cc5f9279c2bfec
blastem_READY = $(blastem_DIR)/COPYING

$(blastem_READY):
	$(call git_fetch,$(blastem_DIR),$(blastem_URL),$(blastem_COMMIT))
	$(MAKE) -C $(blastem_DIR) CPU=mips OS=Linux PYTHON=$(PYTHON) m68k.c z80.c upd78k2.c sh2.c rom.db.c systems.cfg.c

blastem_OBJS = system genesis vdp io romdb hash xband realtec i2c nor 68kinst disasm m68k sega_mapper multi_game \
    megawifi net serialize terminal config tern util paths gst gen backend mem arena ym2612 ymf262 ym_common psg \
    wave flac vgm event_log render_audio rf5c164 saves jcart gen_player coleco pico_pcm ymz263b segacd lc8951 \
    cdimage chdimage libchdr/libchdr_bitstream libchdr/libchdr_cdrom libchdr/libchdr_chd libchdr/libchdr_flac \
    libchdr/libchdr_huffman libchdr/zstd_stub lzma/LzmaDec cdd_mcu cd_graphics cdd_fader sft_mapper mediaplayer \
    laseractive upd78k2_dis upd78k2 osd_font pd0178 radica 32x 32x_video sh2 sh2_decode sh7095 sms i8255 \
    korean_sms_multi z80 z80inst libblastem lib_stubs rom.db systems.cfg vfs_file \
    zlib/adler32 zlib/compress zlib/crc32 zlib/deflate zlib/gzclose zlib/gzlib zlib/gzread zlib/gzwrite zlib/infback \
    zlib/inffast zlib/inflate zlib/inftrees zlib/trees zlib/uncompr zlib/zutil
blastem_SRCS = $(blastem_OBJS:%=$(blastem_DIR)/%.c)
# The sources exist only once fetched (and generated, for m68k.c, z80.c and the others).
$(blastem_SRCS): $(blastem_READY) ;

blastem_INC = -I$(blastem_DIR)
# Upstream's CFLAGS for the generated cores (-DNEW_CORE -DNEW_Z80) and library
# build (-DIS_LIB -DDISABLE_NUKLEAR), plus its CHD include paths; no LTO, no -fPIC.
blastem_CFLAGS = -std=gnu99 -O2 -DNDEBUG -w -DHAS_PROC -DHAVE_UNISTD_H -DNEW_CORE -DNEW_Z80 -DIS_LIB -DDISABLE_NUKLEAR \
                 -I$(blastem_DIR) -I$(blastem_DIR)/libchdr/include -I$(blastem_DIR)/libchdr -I$(blastem_DIR)/lzma \
                 -I$(blastem_DIR)/zlib
blastem_CXXFLAGS = $(blastem_CFLAGS)
