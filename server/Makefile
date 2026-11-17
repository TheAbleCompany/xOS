# ============================================================
# server/Makefile -- WindowServer (userspace ELF)
# ============================================================
#
# wserver.bin собирается так же, как приложения (apps/Makefile):
# freestanding, -nostdlib, -mno-sse (FPU-состояние планировщик
# не сохраняет).  Кладётся в initrd с program_type = gui.
#
# Точка входа: _start в wserver.c.

CC = gcc
LD = ld

COMMON_CFLAGS = \
	-m64 \
	-nostdlib \
	-ffreestanding \
	-fno-stack-protector \
	-fPIE \
	-fpie \
	-O2 \
	-Wall \
	-Wextra \
	-Wno-incompatible-pointer-types \
	-I. \
	-I../lib \
	-I../drivers

CFLAGS = $(COMMON_CFLAGS)
# SSE включён: планировщик сохраняет FPU-состояние (fxsave/fxrstor),
# TrueType-движку нужен float-ABI (xmm0).

LDFLAGS = \
	-m elf_x86_64 \
	-nostdlib \
	-e _start \
	-static \
	-pie

OBJS = \
	wserver.o \
	wserver_surface.o \
	wserver_menu.o \
	wserver_ui.o \
	wserver_input.o \
	wserver_wallpaper.o \
	wserver_launch.o \
	wserver_lion.o \
	wserver_gfx.o \
	wserver_axgl.o \
	wserver_libc_glue.o \
	libc_srv.o

all: wserver.bin

wserver.bin: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

wserver.o: wserver.c wserver_internal.h wserver_gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_surface.o: wserver_surface.c wserver_internal.h wserver_gfx.h ../lib/axport.h ../lib/axipc.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_menu.o: wserver_menu.c wserver_internal.h wserver_gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_ui.o: wserver_ui.c wserver_internal.h wserver_gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_input.o: wserver_input.c wserver_internal.h wserver_gfx.h ../drivers/keyboard_map.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_wallpaper.o: wserver_wallpaper.c wserver_internal.h wserver_gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_launch.o: wserver_launch.c wserver_internal.h wserver_gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_lion.o: wserver_lion.c wserver_internal.h wserver_gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_gfx.o: wserver_gfx.c wserver_gfx.h ../lib/font8x8_basic.h ../lib/axport.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_axgl.o: wserver_axgl.c wserver_internal.h axgl_server.h
	$(CC) $(CFLAGS) -c $< -o $@

wserver_libc_glue.o: wserver_libc_glue.c wserver_internal.h wserver_gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

# СВОЯ копия libc.o: прекомпилированный ../lib/libc.o собран
# без -fPIE (для статических приложений) и в PIE-линк не годится.
libc_srv.o: ../lib/libc.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o wserver.bin
