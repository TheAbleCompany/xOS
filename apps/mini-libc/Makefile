NASM = nasm
CPPFLAGS = -nostdinc -Iinclude
CFLAGS = -Wall -Wextra -fno-stack-protector -fno-builtin -fPIE
# Remove the line below to disable debugging support.
CFLAGS += -O2

# Метка времени сборки libc (для time()/__DATE__/__TIME__ в tcc:
# в ядре ArtyomX64 нет RTC с эпохой, см. time/time.c).
TIME_EPOCH := $(shell date +%s)
CFLAGS += -DTIME_EPOCH=$(TIME_EPOCH)

# Сборка под хост-Linux с шимом syscall (тесты компилятора на хосте):
#   make SHIM=1          -> libc-shim.a + объекты в build-shim/
SHIM =
ifeq ($(SHIM),1)
CFLAGS += -DARTYOM_HOST_SHIM=1
# На хосте входом управляет crt1.o glibc: переименовываем наш
# __libc_start_main, чтобы его не вытянул из архива хост-линкер
# (сигнатуры несовместимы — иначе мгновенный segfault на старте).
CFLAGS += -D__libc_start_main=artyom_libc_start_main
OBJDIR := build-shim
LIBSUF := -shim.a
else
OBJDIR := .
LIBSUF := .a
endif

.PHONY: all clean pack

SRCS = syscall.c \
	   glibc_compat.c \
	   process/exit.c process/sleep.c \
	   mm/malloc.c mm/mmap.c mm/mem_list.c \
	   string/string.c string/strdup.c \
	   stat/fstatat.c stat/fstat.c stat/stat.c \
	   io/open.c io/close.c io/read_write.c io/puts.c io/printf.c io/fgetc.c \
	   io/fputc.c io/putc.c io/fflush.c \
	   io/lseek.c io/truncate.c io/ftruncate.c \
	   io/fopen.c io/std.c io/ungetc.c io/fileops.c io/scanf.c \
	   io/unlink.c io/system.c io/fseek.c io/mkstemp.c io/getdents.c \
	   stdlib/strtod.c \
	   stdlib/strtoll.c \
	   stdlib/strtof.c \
	   stdlib/qsort.c \
	   stdlib/bsearch.c \
	   stdlib/env.c \
	   stdlib/abort.c \
	   stdlib/atexit.c \
	   stdlib/assert.c \
	   stdlib/misc.c \
	   stdlib/rand.c \
	   ctype/ctype.c \
	   math/math.c \
	   time/time.c \
	   errno.c \
	   crt/__libc_start_main.c \
	   crt/jmp.S \
	   crt/start.S

# stdlib/strtoull.c исключён: strtoull теперь в stdlib/strtoll.c
# (старая реализация была некорректной и дублировала __isoc23_strtoull).
#
# ВАЖНО: в старом Makefile crt/jmp.S и crt/start.S попадали в архив
# ИСХОДНИКАМИ (patsubst %.c не трогал .S) — теперь оба собираются в .o.

ifeq ($(SHIM),1)
OBJS = $(addprefix $(OBJDIR)/,$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(SRCS))))
else
OBJS = $(patsubst %.S,%.o,$(patsubst %.c,%.o,$(SRCS)))
endif

all: libc$(LIBSUF)

libc$(LIBSUF): $(OBJS)
	$(AR) -rc $@ $^

ifeq ($(SHIM),1)
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
else
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
endif

pack: clean
	zip -r ../src.zip *

clean:
	-rm -rf build-shim
	-rm -f *~
	-rm -f $(OBJS)
	-rm -f libc.a libc-shim.a
