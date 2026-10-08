#!/bin/sh
# Builds build/opengl32.dll (32-bit) with MinGW-w64.
# Needs: python3, i686-w64-mingw32-gcc (Debian/Ubuntu: gcc-mingw-w64-i686; MSYS2: mingw-w64-i686-gcc).
set -e
cd "$(dirname "$0")"
CC=${CC:-i686-w64-mingw32-gcc}
mkdir -p build
python3 gen_exports.py exports.txt build
$CC -O2 -Wall -Wextra -Ibuild -shared -static-libgcc -Wl,--enable-stdcall-fixup \
    -o build/opengl32.dll glfix.c build/stubs.S build/opengl32.def -luser32
${STRIP:-i686-w64-mingw32-strip} build/opengl32.dll
echo "built $(pwd)/build/opengl32.dll"
