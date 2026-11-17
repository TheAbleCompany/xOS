# Makefile для Lua (ArtyomX64 / mini-libc)
#
# ВАЖНО (зависание при запуске!):
# Загрузчик ELF ядра ArtyomX64 (multitask/loader.c) размещает образ
# по произвольному адресу (kmalloc) и умеет обрабатывать только
# динамические R_*_RELATIVE/GLOB_DAT/64-релокации ET_DYN-образа.
# Поэтому приложение обязано быть статическим PIE:
#   - компиляция: -fPIC (НЕ -fno-PIC / -no-pie!)
#   - линковка:   -static-pie (тип ET_DYN, база 0, RELA)
# ET_EXEC с фиксированной базой (0x400000) не работает: абсолютные
# ссылки останутся указывать на 0x400000 -> page fault -> зависание.
CC = cc
CFLAGS = -Wall -Os -fno-asynchronous-unwind-tables -fPIC -fno-stack-protector -fno-builtin -g0 -nostdinc -I../mini-libc/include
LDFLAGS = -nostdlib ../mini-libc/libc.a -static-pie -Wl,--build-id=none -Wl,-z,noseparate-code

# mathlib.o входит в сборку (математика реализована в mini-libc/math),
# -lm убран: libm хоста несовместима с -nostdlib.
OBJS = hash.o inout.o lex_yy.o opcode.o table.o y_tab.o lua.o iolib.o strlib.o mathlib.o

all: lua

lua: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) lua

.PHONY: all clean
