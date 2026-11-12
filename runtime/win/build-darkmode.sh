#!/bin/sh
# Build runtime/third_party/darkmodelib/src/*.cpp as a static library for the
# Windows host: build-darkmode.sh <c++ compiler> <archiver> <output folder>
#
# The sources are fetched by fetch-darkmodelib.sh (they are not in git). The
# flags are upstream's Makefile (C++20, _WIN64, Unicode, Windows 7 headers)
# with its INI reader off (the host has no darkmode.ini) and its Windows-heap
# allocator on, so the library has no CRT-internal settings of its own.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
"$H/fetch-darkmodelib.sh"
D="$H/../third_party/darkmodelib"
CXX="$1"; AR="$2"; OUT="$3"
[ -n "$CXX" ] && [ -n "$AR" ] && [ -n "$OUT" ] || {
    echo "usage: build-darkmode.sh <c++ compiler> <archiver> <output folder>" >&2; exit 2; }
OBJ="$OUT/darkmode-obj"
mkdir -p "$OBJ"
FLAGS="-std=c++20 -O2 -DNDEBUG -D_WIN64 -D_WINDOWS -DSTRICT_TYPED_ITEMIDS -DNOMINMAX
  -DWIN32_LEAN_AND_MEAN -DVC_EXTRALEAN -DSUPPORT_UTF8 -DUNICODE -D_UNICODE
  -D_WIN32_WINNT=0x0601 -D_DARKMODELIB_NO_INI_CONFIG -D_DARKMODELIB_CUSTOM_MEM=0x002
  -I$D/include"
for f in "$D"/src/*.cpp; do
    o="$OBJ/$(basename "$f" .cpp).o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$H/build-darkmode.sh" -nt "$o" ]; then
        echo "  c++ $(basename "$f")"
        $CXX $FLAGS -c "$f" -o "$o"
    fi
done
rm -f "$OUT/libdarkmode.a"
$AR rcs "$OUT/libdarkmode.a" "$OBJ"/*.o
echo "built $OUT/libdarkmode.a"
