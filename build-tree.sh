#!/bin/sh
# Build harness. Toolchain: clang (ELF target) + lld, both cross-driving;
# make from MinGW. Nothing here overrides project flags -- it only adds
# the host tools to PATH and sets CC/AR.
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
export PATH="/d/toolchain/bin:/d/MinGW/bin:/c/Users/User/AppData/Local/bin/NASM:$PATH"
export CC="${CC:-cc}"
export AR=ar
cd "$ROOT"

case "${1:-all}" in
  clean)  exec mingw32-make clean ;;
  kernel) exec mingw32-make main.elf ;;
  apps)   exec mingw32-make -C apps initrd ;;
  iso)    exec sh build-iso.sh ;;
  run)    exec mingw32-make run-limine ;;
  *)      mingw32-make
          mingw32-make -C apps initrd
          exec sh build-iso.sh
          ;;
esac
