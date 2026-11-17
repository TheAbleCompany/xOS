# ============================================================
# Quake (TyrQuake 0.71, NQ + программный рендер) для ArtyomX64
# ============================================================
#
# Собирается как обычное userspace-приложение ОС: PIE-бинарь,
# линкуется с apps/mini-libc/libc.a, окно -- через WindowServer
# (apps/quake/axglue.h).  Платформенный слой:
#
#   vid_artyom.c  -- 320x200 8bpp -> 640x400 ARGB в канвас
#   in_artyom.c   -- сканкоды wserver'а + мышь
#   sys_artyom.c  -- время (сисколл 196), COM1, main()
#   net_none.c    -- только loopback-драйвер (сеть не нужна)
#   snd_null.c    -- без звука
#
#   make            -> ../quake.bin
#   make clean
# ============================================================

CC = gcc
LD = ld

CFLAGS = \
	-m64 \
	-nostdlib \
	-ffreestanding \
	-fno-stack-protector \
	-fno-builtin \
	-fPIE \
	-fpie \
	-O2 \
	-msse2 \
	-mfpmath=sse \
	-nostdinc \
	-MMD -MP \
	-Icompat \
	-I../mini-libc/include \
	-Iinclude \
	-INQ \
	-I. \
	-I../../lib \
	-DNQ_HACK -DELF -DNDEBUG \
	-DSEEK_SET=0 -DSEEK_CUR=1 -DSEEK_END=2 \
	-Dalloca=__builtin_alloca \
	-DTYR_VERSION=0.71 -DTYR_VERSION_TIME=0LL \
	-w

LDFLAGS = \
	-m elf_x86_64 \
	-nostdlib \
	-e _start \
	-static \
	-pie

OUT = ../quake.bin
LIBC = ../mini-libc/libc.a

COMMON = \
	common/cmd.c common/common.c common/crc.c common/cvar.c \
	common/mathlib.c common/model.c common/rb_tree.c common/shell.c \
	common/zone.c \
	common/alias_model.c common/cd_common.c common/cd_null.c \
	common/cl_input.c common/console.c common/developer.c common/keys.c \
	common/menu.c common/pcx.c common/r_efrag.c common/r_lerp.c \
	common/r_light.c common/r_model.c common/r_part.c \
	common/screen.c common/snd_dma.c common/snd_mem.c common/snd_mix.c \
	common/snd_null.c common/sprite_model.c \
	common/tga.c common/vid_mode.c common/wad.c

SERVER = \
	common/pr_cmds.c common/pr_edict.c common/pr_exec.c \
	common/sv_move.c common/sv_phys.c

SOFTWARE = \
	common/d_edge.c common/d_fill.c common/d_init.c common/d_modech.c \
	common/d_part.c common/d_polyse.c common/d_scan.c common/d_sky.c \
	common/d_sprite.c common/d_surf.c common/d_vars.c \
	common/draw.c common/r_aclip.c common/r_alias.c common/r_bsp.c \
	common/r_draw.c common/r_edge.c common/r_main.c common/r_misc.c \
	common/r_sky.c common/r_sprite.c common/r_surf.c common/r_vars.c \
	common/world.c

NQ = \
	NQ/chase.c NQ/cl_demo.c NQ/cl_main.c NQ/cl_parse.c NQ/cl_tent.c \
	NQ/host.c NQ/host_cmd.c NQ/net_common.c \
	NQ/net_loop.c NQ/net_main.c NQ/net_none.c \
	NQ/sbar.c NQ/sv_main.c NQ/sv_user.c NQ/view.c

PLATFORM = \
	axglue.c vid_artyom.c in_artyom.c sys_artyom.c music_artyom.c \
	net_artyom.c buildinfo_artyom.c compat_artyom.c

SRCS = $(COMMON) $(SERVER) $(SOFTWARE) $(NQ) $(PLATFORM)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

all: $(OUT)

$(OUT): $(OBJS) $(LIBC)
	$(LD) $(LDFLAGS) $(OBJS) $(LIBC) -o $@
	@ls -l $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(OUT) $(DEPS)

.PHONY: all clean

# Автозависимости от заголовков (пишутся -MMD рядом с .o).
# Строго в конце файла: первый target в makefile становится целью по
# умолчанию, а .d-файлы содержат правила вида "foo.o: foo.c ...".
-include $(DEPS)
