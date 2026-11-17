#!/bin/sh
# tests-host/run_tests.sh — гоняет тесты mini-libc на хосте через шим.
# Методика порта lua: весь код собирается с -DARTYOM_HOST_SHIM=1,
# номера syscall ArtyomX64 транслируются shim'ом в реальные Linux-вызовы.
set -e
cd "$(dirname "$0")"

LIBC=../mini-libc
OUT=bin
CC=gcc

CFLAGS="-nostdinc -I$LIBC/include -fno-stack-protector -fno-builtin \
        -fPIE -O2 -DARTYOM_HOST_SHIM=1 -Wno-unused-result"

mkdir -p $OUT

# Пересборка libc-shim.a
make -C $LIBC SHIM=1 >/dev/null

# Шим -> обычный объект (тут уже glibc-заголовки)
$CC -c syscall_shim.c -o $OUT/syscall_shim.o

run_test() {
        name=$1
        echo "=== $name ==="
        $CC $CFLAGS -c $name.c -o $OUT/$name.o
        $CC -no-pie $OUT/$name.o $OUT/syscall_shim.o \
            $LIBC/libc-shim.a -o $OUT/$name
        ./$OUT/$name
        echo
}

run_test test_setjmp
run_test test_libc2

echo "ALL TEST SUITES DONE"
