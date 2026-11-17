#!/bin/sh
#
# build-artyomx64.sh — сборка tcc для ArtyomX64 (+ хост-шим для тестов).
#
# Результат в build-art/:
#   tcc            — компилятор для ОС (статический ELF, int 0x80, mini-libc)
#   libtcc1.a      — рантайм-хелперы tcc (собраны gcc, тот же SysV ABI)
#   crt0.o         — стартовый объект (_start + __libc_start_main) для -r линковки
#   include/       — системные заголовки для пользовательских программ на ОС
#   tcc-shim       — тот же tcc, но с syscall-шимом для тестов на хосте Linux
#
# Пути: формат initrd плоский, всё лежит в корне "/".
#

set -e
cd "$(dirname "$0")"

LIBC=../mini-libc
OUT=build-art
SHIM=../tests-host

CC=gcc

# Флаги компиляции самого tcc (источники + config.h этого каталога).
WARN="-Wno-unused-result -Wno-error=implicit-function-declaration \
      -Wno-incompatible-pointer-types -Wno-implicit-int"
BASE="-O2 -fno-stack-protector -fno-builtin -fno-PIE $WARN"
INC="-nostdinc -I$LIBC/include -I."

rm -rf $OUT
mkdir -p $OUT/bin $OUT/include

echo "=== [1/6] mini-libc (реальная, int 0x80) ==="
make -C $LIBC clean >/dev/null
make -C $LIBC >/dev/null

echo "=== [2/6] tcc: реальная цель ArtyomX64 ==="
$CC $INC $BASE -DONE_SOURCE -c tcc.c -o $OUT/tcc.o
$CC -no-pie -nostdlib -o $OUT/bin/tcc \
    $OUT/tcc.o $LIBC/crt/start.o $LIBC/libc.a

echo "=== [3/6] tcc: хост-шим (тесты на Linux) ==="
$CC $INC $BASE -DONE_SOURCE -DARTYOM_HOST_SHIM=1 \
    -D__libc_start_main=artyom_libc_start_main \
    -c tcc.c -o $OUT/tcc-shim.o
make -C $LIBC SHIM=1 >/dev/null
$CC -c $SHIM/syscall_shim.c -o $OUT/syscall_shim.o
$CC -no-pie -o $OUT/bin/tcc-shim \
    $OUT/tcc-shim.o $OUT/syscall_shim.o $LIBC/libc-shim.a

echo "=== [4/6] libtcc1.a (gcc, SysV ABI) ==="
LIBFLAGS="-O2 -fno-stack-protector -fno-builtin -fno-PIE -I. -Wno-unused-variable"
for f in lib/libtcc1.c lib/stdatomic.c lib/builtin.c lib/va_list.c; do
        $CC $LIBFLAGS -c $f -o $OUT/$(basename $f .c).o
done
$CC $LIBFLAGS -c lib/alloca.S -o $OUT/alloca.o
$CC $LIBFLAGS -c lib/atomic.S -o $OUT/atomic.o
rm -f $OUT/libtcc1.a
ar -rc $OUT/libtcc1.a \
        $OUT/libtcc1.o $OUT/stdatomic.o $OUT/builtin.o \
        $OUT/va_list.o $OUT/alloca.o $OUT/atomic.o

echo "=== [5/6] crt0.o (ld -r: _start + __libc_start_main) ==="
ld -r -o $OUT/crt0.o $LIBC/crt/start.o $LIBC/crt/__libc_start_main.o

echo "=== [6/6] системные заголовки для ОС ==="
# На ОС заголовки лежат в корне initrd ("{B}" = "/").
# <sys/stat.h> упаковывается в initrd файлом с именем "sys/stat.h"
# (формат плоский, но ядро сматчит имя целиком, см. патч skip_leading_slashes).
# Для хост-тестов делаем настоящий подкаталог sys/.
INC=$OUT/include
mkdir -p $INC/sys
for h in stdio.h stdlib.h string.h ctype.h errno.h limits.h math.h \
         setjmp.h time.h unistd.h fcntl.h stdint.h stdbool.h stddef.h \
         inttypes.h assert.h; do
        cp $LIBC/include/$h $INC/
done
for h in stat.h types.h mman.h time.h; do
        cp $LIBC/include/sys/$h $INC/sys/$h
done
# tcc (stdarg/stddef на __builtin_va_list — родной для tcc)
for h in stdarg.h float.h tgmath.h varargs.h stdalign.h stdnoreturn.h; do
        cp include/$h $INC/
done
mkdir -p $INC/internal
for h in types.h essentials.h io.h; do
        cp $LIBC/include/internal/$h $INC/internal/
done
# предопределения tcc (CONFIG_TCC_PREDEFS=0: читаются в рантайме)
cp include/tccdefs.h $INC/

echo "=== ГОТОВО: build-art/ ==="
ls -la $OUT $OUT/bin
