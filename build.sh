#!/usr/bin/env bash
# Builds dist/VozAlien.asi (32-bit Windows DLL) with mingw-w64.
#   Arch:   pacman -S mingw-w64-gcc
#   Debian/Ubuntu: apt install gcc-mingw-w64-i686
#   Windows: MSYS2 "MINGW32" shell -> pacman -S mingw-w64-i686-gcc, then: CC=gcc ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
CC="${CC:-i686-w64-mingw32-gcc}"
mkdir -p dist
"$CC" -O2 -Wall -Wextra -Wno-format-truncation -shared -static-libgcc -s \
  -o dist/VozAlien.asi src/voz_alien.c -lwinmm -lm
cp VozAlien.ini dist/VozAlien.ini
echo "ok: dist/VozAlien.asi"
