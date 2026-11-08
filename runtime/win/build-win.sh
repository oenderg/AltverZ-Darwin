#!/bin/sh
# Build the Windows ARM64 programs. ./build-win.sh starttest   (more targets as they exist)
# Objects go to out/win-obj and programs to out/win in the repo; the link takes an explicit
# list of objects, so stale ones there are never linked.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
VERSION="$(cat "$REPO/VERSION")"
TC="${TL_WIN_TOOLCHAIN:?set TL_WIN_TOOLCHAIN to the folder holding llvm-mingw-* and win-arm64 (see runtime/win/README.md)}"
CC="$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/aarch64-w64-mingw32-clang"
CXX="$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/aarch64-w64-mingw32-clang++"
AR="$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/llvm-ar"
OBJ="$REPO/out/win-obj"; BIN="$REPO/out/win"
mkdir -p "$OBJ" "$BIN"
N="$H/../src/translation-layer-next"; T="$H/../src/translation-layer"
. "$H/sources.sh"
target="${1:-starttest}"
case "$target" in
    starttest) SRCS="$WIN_CORE $H/win-starttest.c"; OUT="$BIN/rfl-starttest.exe" ;;
    game) SRCS="$WIN_CORE $WIN_GAME"; OUT="$BIN/PvZ2 Reflourished.exe"; LDX="-mwindows -lwinmm -lshell32 -lole32 -lcomctl32 -lwinhttp -lgdiplus -ldwmapi -luxtheme -lshlwapi -lcomdlg32" ;;
    *) echo "unknown target $target" >&2; exit 2 ;;
esac
OBJS=""
for f in $SRCS; do
    o="$OBJ/$(basename "$f" | sed 's/\.c$//').o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$REPO/VERSION" -nt "$o" ] || [ "$H/../tools/pvz2-windows-settings.h" -nt "$o" ] || [ "$H/compat/win-compat.h" -nt "$o" ]; then
        "$H/cc.sh" "$f" "-DPVZ2_APP_VERSION=\"$VERSION\"" -c -o "$o"
    fi
    OBJS="$OBJS $o"
done
if [ "$target" = game ]; then
    RC="$OBJ/win-manifest.o"
    if [ ! -f "$RC" ] || [ "$H/win-manifest.rc" -nt "$RC" ] || [ "$H/win-manifest.xml" -nt "$RC" ]; then
        "$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/llvm-windres" -I"$H" -O coff \
            -i "$H/win-manifest.rc" -o "$RC"
    fi
    OBJS="$OBJS $RC"
    # The dark mode library is C++ and has its own flags (runtime/win/build-darkmode.sh);
    # the shim is its only C++ file in our code.
    "$H/build-darkmode.sh" "$CXX" "$AR" "$OBJ"
    DM="$OBJ/pvz2-windows-darkmode.o"
    DMSRC="$H/../tools/pvz2-windows-darkmode.cpp"
    if [ ! -f "$DM" ] || [ "$DMSRC" -nt "$DM" ] || [ "$H/../tools/pvz2-windows-darkmode.h" -nt "$DM" ]; then
        "$CXX" -std=c++20 -O2 -g -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
            -I"$H/../third_party/darkmodelib/include" -I"$H/../tools" -c "$DMSRC" -o "$DM"
    fi
    OBJS="$OBJS $DM $OBJ/libdarkmode.a"
fi
"$CXX" -static -o "$OUT" $OBJS $LDX -L"$TC/win-arm64/lib" -lz -lz-guest -lpsapi -lws2_32 -lbcrypt -lsynchronization -lpthread -Wl,--error-limit=0
echo "built $OUT"
