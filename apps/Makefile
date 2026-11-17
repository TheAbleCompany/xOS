CC = gcc
LD = ld

# ============================================================
# Общие флаги
# ============================================================

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
	-I../lib

LDFLAGS = \
	-m elf_x86_64 \
	-nostdlib \
	-e _start \
	-static \
	-pie


# ============================================================
# Обычные приложения
# ============================================================

# SSE включён: планировщик сохраняет FPU-состояние (fxsave/fxrstor
# в multitask/switch.S), а TrueType-движок (stb_truetype) возвращает
# float, что по ABI x86-64 требует xmm0.
CFLAGS = $(COMMON_CFLAGS)


# ============================================================
# Raycaster
#
# Использует float/sin/cos/sqrt.
# ============================================================

RAYCASTER_CFLAGS = $(COMMON_CFLAGS) \
	-msse2 \
	-mfpmath=sse \
	-fno-math-errno \
	-fno-builtin-sincosf


# ============================================================
# libc
# ============================================================

LIBC_SRC = ../lib/libc.c
LIBC_OBJ = libc.o


# ============================================================
# Приложения
# ============================================================

APPS = \
	prefs.bin \
	calc.bin \
	files.bin \
	notes.bin \
	temu.bin \
	raycaster.bin \
	cube.bin \
	guitest.bin \
	test \
	bench.bin \
	wserver.bin \
	doom.bin \
	quake.bin \
	coreutils

# ============================================================
# Build
# ============================================================

all: $(APPS)
#lua.copy

# lua-1.0 собирается в lua-1.0-master (см. README-PORT.md);
# сюда копируем готовый бинарник для mkinitrd2.
#lua.copy: lua-1.0-master/lua
#       cp -f lua-1.0-master/lua lua

#lua-1.0-master/lua:
#       $(MAKE) -C lua-1.0-master


# ============================================================
# libc
# ============================================================

libc.o: $(LIBC_SRC)
	$(CC) $(CFLAGS) -c $< -o $@


# ============================================================
# Raycaster
# ============================================================

raycaster.o: raycaster.c
	$(CC) $(RAYCASTER_CFLAGS) -c $< -o $@

raycaster.bin: raycaster.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@


# ============================================================
# Cube -- AxGL demo (OpenGL-style, GPU ixg / software fallback)
#
# Использует float/матрицы -- собирается с SSE2, как raycaster.
# ============================================================

CUBE_CFLAGS = $(RAYCASTER_CFLAGS)

cube.o: cube.c axgl.h
	$(CC) $(CUBE_CFLAGS) -c $< -o $@

cube.bin: cube.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@


# ============================================================
# xsh — GUI-терминал (запуск приложений с аргументами:
# exec lua /comprehensive.lua)
# ============================================================

#xsh.bin: xsh.o libc.o
#       $(LD) $(LDFLAGS) $^ -o $@


# ============================================================
# Остальные приложения
# ============================================================

prefs.bin: prefs.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@

calc.bin: calc.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@

files.bin: files.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@

guitest.bin: guitest.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@

notes.bin: notes.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@

temu.bin: temu.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@

# DOOM (doomgeneric) -- отдельный подкаталог, свой Makefile и mini-libc.
doom.bin:
	$(MAKE) -C doom

# Quake (TyrQuake NQ, software renderer) -- отдельный подкаталог.
quake.bin:
	$(MAKE) -C quake

# WindowServer собирается в ../server своим Makefile, сюда кладётся
# копией -- mkinitrd2 бандлит его как program_type=gui.
wserver.bin: FORCE
	$(MAKE) -C ../server
	cp -f ../server/wserver.bin $@

# Консольный тест кадра (lib/libc.c + WindowServer).
test: test.o libc.o
	$(LD) $(LDFLAGS) $^ -o $@

# Консольные утилиты из sources (ls/rm/cat/echo/hello на mini-libc).
coreutils: FORCE
	$(MAKE) -C coreutils

# ============================================================
# bench -- бенчмарк ОС (mini-libc: main(argc,argv) + printf)
# ============================================================

BENCH_CFLAGS = $(CFLAGS) \
    -U_FORTIFY_SOURCE \
    -D_FORTIFY_SOURCE=0 \
    -fno-builtin \
    -I../mini-libc/include

bench.bin: FORCE
	$(CC) $(BENCH_CFLAGS) -c bench.c -o bench.o
	$(LD) $(LDFLAGS) bench.o mini-libc/libc.a -o bench.bin
	rm -f bench.o

# Linux reference; deliberately excluded from APPS/initrd.
bench-linux: bench_linux.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

FORCE:

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


# ============================================================
# Initrd
# ============================================================

# tcc (порт 2026): компилятор + его библиотеки + системные заголовки.
# Сборка целиком делается build-artyomx64.sh (см. tinycc-mob/README-TCC.md).
TCCDIR := tinycc-mob/build-art

tcc.build:
	$(MAKE) -C tinycc-mob build-art/bin/tcc

TCC_HDR = \
	stdio.h stdlib.h string.h ctype.h errno.h limits.h math.h \
	setjmp.h time.h unistd.h fcntl.h stdint.h stdbool.h stddef.h \
	stdarg.h float.h tgmath.h varargs.h stdalign.h stdnoreturn.h \
	inttypes.h assert.h tccdefs.h

TCC_SYS_HDR = stat.h types.h mman.h time.h
TCC_INT_HDR = internal/types.h internal/essentials.h internal/io.h

initrd: all
	python3 ../tools/make_initrd.py \
	wserver.bin wserver gui \
	temu.bin temu gui \
	notes.bin notes gui \
	prefs.bin prefs gui \
	files.bin files gui \
	guitest.bin guitest gui \
	calc.bin calc gui \
	raycaster.bin raycaster gui \
	cube.bin cube gui \
	doom.bin doom gui \
	quake.bin quake gui \
	doom1.wad doom1.wad none \
	coreutils/ls ls console \
	coreutils/rm rm console \
	coreutils/mkdir mkdir console \
	coreutils/cat cat console \
	coreutils/echo echo console \
	bliss.bmp wp1.bmp none \
	test.txt test.txt none \
	test2.txt test2.txt none \
	test test console \
	bench.bin bench console \
	font.ttf font.ttf none \
	fontmono.ttf fontmono.ttf none \
	hello_tcc_demo.c hello.c none \
	sjt.c sjt.c none \
	coreutils/run run console \
	coreutils/ping ping console \
	stories260K stories none \
	tok512 tok none \
	
	cp initrd.img ../initrd.img
	mkdir -p ../limine-iso-root/boot
	cp initrd.img ../limine-iso-root/boot/initrd.img


# ============================================================
# Clean
# ============================================================

clean:
	rm -f *.o *.bin bench-linux lua initrd.img
