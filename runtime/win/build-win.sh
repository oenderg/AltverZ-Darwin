#!/bin/sh
# Build the Windows ARM64 programs. ./build-win.sh starttest   (more targets as they exist)
# Objects go to out/win-obj and programs to out/win in the repo; the link takes an explicit
# list of objects, so stale ones there are never linked.
#
# Targets:
#   game           the installed build: links WinSparkle and carries its feed and key
#   game-portable  the zip build: no updater at all (out/win-portable)
#   starttest, settings-test   the small tests
#
# APP_VERSION overrides VERSION for a local test build (an older version, to prove an
# update); its objects and programs go to their own folders.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
VERSION="${APP_VERSION:-$(cat "$REPO/VERSION")}"
SUF=""; [ -n "$APP_VERSION" ] && SUF="-$APP_VERSION"
TC="${TL_WIN_TOOLCHAIN:?set TL_WIN_TOOLCHAIN to the folder holding llvm-mingw-* and win-arm64 (see runtime/win/README.md)}"
CC="$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/aarch64-w64-mingw32-clang"
CXX="$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/aarch64-w64-mingw32-clang++"
AR="$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/llvm-ar"
OBJ="$REPO/out/win-obj$SUF"; BIN="$REPO/out/win$SUF"
WS="$REPO/prebuilt/winsparkle"
# The Windows appcast and the EdDSA key of the Mac app's signature (the same key signs
# both, docs/windows-shipping.md). A test build passes its own with SPARKLE_FEED_URL and
# SPARKLE_PUBLIC_KEY; an empty key leaves the updater off.
FEED="${SPARKLE_FEED_URL:-https://raw.githubusercontent.com/oenderg/Reflourished-Darwin/main/sparkle/windows-appcast.xml}"
KEY="${SPARKLE_PUBLIC_KEY:-$(cat "$REPO/sparkle/public-key" 2>/dev/null || true)}"
mkdir -p "$OBJ" "$BIN"
N="$H/../src/translation-layer-next"; T="$H/../src/translation-layer"
. "$H/sources.sh"
target="${1:-starttest}"
# Only the host and the updates module are built differently for the two programs; every
# other object is shared, so a portable build costs two compiles, not a full rebuild.
case "$target" in
    starttest) SRCS="$WIN_CORE $H/win-starttest.c"; OUT="$BIN/rfl-starttest.exe" ;;
    game) SRCS="$WIN_CORE $WIN_GAME"; OUT="$BIN/PvZ2 Reflourished.exe"
        LDX="-mwindows -lwinmm -lshell32 -lole32 -lcomctl32 -lwinhttp -lgdiplus -ldwmapi -luxtheme -lshlwapi -lcomdlg32" ;;
    game-portable) SRCS="$WIN_CORE $WIN_GAME"; OUT="$REPO/out/win-portable$SUF/PvZ2 Reflourished.exe"
        LDX="-mwindows -lwinmm -lshell32 -lole32 -lcomctl32 -lwinhttp -lgdiplus -ldwmapi -luxtheme -lshlwapi -lcomdlg32" ;;
    settings-test) SRCS="$H/win-settings-test.c $H/../tools/pvz2-windows-settings.c $H/../tools/pvz2-windows-updates.c"; OUT="$BIN/rfl-settings-test.exe"; LDX="-lshell32 -lcomctl32 -lgdi32 -ldwmapi -luxtheme -lshlwapi -lcomdlg32" ;;
    *) echo "unknown target $target" >&2; exit 2 ;;
esac
UPD_DEFS="-DPVZ2_UPDATES=0"
[ "$target" = game ] && UPD_DEFS="-DPVZ2_UPDATES=1 -DPVZ2_UPDATE_FEED_URL=\"$FEED\" -DPVZ2_UPDATE_PUBLIC_KEY=\"$KEY\""
UPD_OBJ="$OBJ/portable"; UPD_EXTRA=""; UPD_LIBS=""
[ "$target" = game ] && { UPD_OBJ="$OBJ/installed"; UPD_EXTRA="-I$WS/include"; UPD_LIBS="-L$WS/arm64 -lWinSparkle"; }
mkdir -p "$UPD_OBJ" "$(dirname "$OUT")"
OBJS=""
for f in $SRCS; do
    case "$f" in
        */pvz2-windows.c|*/pvz2-windows-updates.c)
            o="$UPD_OBJ/$(basename "$f" | sed 's/\.c$//').o"
            flags="-DPVZ2_APP_VERSION=\"$VERSION\" $UPD_DEFS $UPD_EXTRA"
            ;;
        *)  o="$OBJ/$(basename "$f" | sed 's/\.c$//').o"
            flags="-DPVZ2_APP_VERSION=\"$VERSION\""
            ;;
    esac
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$REPO/VERSION" -nt "$o" ] || [ "$H/../tools/pvz2-windows-settings.h" -nt "$o" ] || [ "$H/../tools/pvz2-windows-updates.h" -nt "$o" ] || [ "$H/compat/win-compat.h" -nt "$o" ]; then
        "$H/cc.sh" "$f" $flags -c -o "$o"
    fi
    OBJS="$OBJS $o"
done
if [ "$target" = game ] || [ "$target" = game-portable ] || [ "$target" = settings-test ]; then
    RC="$OBJ/win-manifest.o"
    if [ ! -f "$RC" ] || [ "$H/win-manifest.rc" -nt "$RC" ] || [ "$H/win-manifest.xml" -nt "$RC" ] || [ "$H/pvz2.ico" -nt "$RC" ]; then
        "$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/llvm-windres" -I"$H" -O coff \
            -i "$H/win-manifest.rc" -o "$RC"
    fi
    OBJS="$OBJS $RC"
fi
if [ "$target" = game ] || [ "$target" = game-portable ] || [ "$target" = settings-test ]; then
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
"$CXX" -static -o "$OUT" $OBJS $LDX $UPD_LIBS -L"$TC/win-arm64/lib" -lz -lz-guest -lpsapi -lws2_32 -lbcrypt -lsynchronization -lpthread -Wl,--error-limit=0
echo "built $OUT"
