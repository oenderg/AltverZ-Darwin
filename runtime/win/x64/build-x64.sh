#!/bin/sh
# Build the x86-64 Windows programs, in MSYS2's CLANG64 shell (clang, LLVM 23):
#   build-x64.sh <runtime folder> <ScarletNX checkout, its library built in build/lib> <zlib folder> <out folder> [starttest|game|game-portable]
#
# Cross-built on a Mac or Linux instead when X64_CROSS_SYSROOT (the folder cross/setup-sysroot.sh made) and
# LLVM_MINGW (the llvm-mingw folder) are set: the tools are llvm-mingw's x86-64 ones, the program links LLVM's
# import library from the sysroot, and its own C++ (the dark mode shim) links libc++ and libunwind statically, so
# the one libc++.dll in the process is the one libLLVM-23.dll brings. runtime/win/build-x64-cross.sh runs the steps.
# <zlib folder> holds zlib-1.3.1 and zlib-guest (setup-toolchain.sh makes both) and include/ with their headers.
#
# game is the installed build: it links WinSparkle from prebuilt/winsparkle/x64 and carries
# the feed and key (SPARKLE_FEED_URL, SPARKLE_PUBLIC_KEY override them for a test). game-portable
# has no updater and writes to <out folder>-portable. APP_VERSION overrides VERSION.
set -e
R="$(cd "$1" && pwd)"; SNX="$(cd "$2" && pwd)"; Z="$(cd "$3" && pwd)"; OUT="$4"; target="${5:-starttest}"
H="$R/win"; N="$R/src/translation-layer-next"; T="$R/src/translation-layer"; X="$H/x64"
# The mod this build is for (data/profiles/<id>.json). Its generated pvz2-profile-generated.h
# (scripts/profile-generate.py, written by build-win.sh into out/win-profile-<id>) is on the include
# path ahead of the checked-in defaults; PROFILE_DIR names that folder. MSYS2 has no Python, so the
# few plain strings read here come straight from the JSON.
PROFILE="${PROFILE:-altverz}"
PROFILE_JSON="$R/../data/profiles/$PROFILE.json"
pj() { sed -n "s/^ *\"$1\": *\"\([^\"]*\)\".*/\1/p" "$PROFILE_JSON" | head -1; }
PROFILE_DIR="${PROFILE_DIR:-$R/../out/win-profile-$PROFILE}"
[ -f "$PROFILE_DIR/pvz2-profile-generated.h" ] || { echo "no $PROFILE_DIR/pvz2-profile-generated.h: run runtime/win/build-win.sh once on a Mac, or profile-generate.py" >&2; exit 1; }
APP_NAME="$(pj app_name)"
VERSION="${APP_VERSION:-$(pj app_version)}"
WS="$R/../prebuilt/winsparkle"
FEED="${SPARKLE_FEED_URL:-$(pj sparkle_feed_url | sed 's|/appcast.xml$|/windows-appcast.xml|')}"
KEY="${SPARKLE_PUBLIC_KEY:-$(cat "$R/../sparkle/public-key" 2>/dev/null || true)}"
OBJ="$OUT/obj"; mkdir -p "$OBJ" "$OUT"
if [ -n "${X64_CROSS_SYSROOT:-}" ]; then
    MB="${LLVM_MINGW:?set LLVM_MINGW to the llvm-mingw folder}/bin"
    CC="$MB/x86_64-w64-mingw32-clang"; CXX="$MB/x86_64-w64-mingw32-clang++"
    AR="$MB/x86_64-w64-mingw32-llvm-ar"; WINDRES="$MB/x86_64-w64-mingw32-windres"
    # the import library by its file: LLVM's DLL is the one library the program takes from the sysroot
    LLVM_LINK="$X64_CROSS_SYSROOT/clang64/lib/libLLVM-23.dll.a"; CXX_RT="-static-libstdc++ -static-libgcc"
    [ -f "$LLVM_LINK" ] || { echo "no $LLVM_LINK: run runtime/win/x64/cross/setup-sysroot.sh" >&2; exit 1; }
else
    CC=clang; CXX=clang++; AR=llvm-ar; WINDRES=windres; LLVM_LINK="-lLLVM-23"; CXX_RT=""
fi
CFLAGS="-std=gnu11 -g -O1 -D_GNU_SOURCE -DTL_WINDOWS=1 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wno-unused-function
  -include $H/compat/win-compat.h -I$H/compat -I$T -I$N -I$R/tools -I$Z/include -I$PROFILE_DIR -I$X -I$SNX/src"

# zlib twice: the runtime's, and the guest's (64-bit uLong, z_ prefixed)
ZS="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil"
if [ ! -f "$OUT/libz-guest.a" ]; then
    mkdir -p "$OBJ/zh" "$OBJ/zg"
    for f in $ZS; do $CC -O2 -c "$Z/zlib-1.3.1/$f.c" -o "$OBJ/zh/$f.o"; done
    for f in $ZS; do $CC -O2 -DZ_PREFIX -Dz_errmsg=tl_guestz_errmsg -c "$Z/zlib-guest/$f.c" -o "$OBJ/zg/$f.o"; done
    $AR rcs "$OUT/libz.a" "$OBJ"/zh/*.o
    $AR rcs "$OUT/libz-guest.a" "$OBJ"/zg/*.o
fi

. "$H/sources.sh"
# Only the host and the updates module differ between the two programs.
UPD_DEFS="-DPVZ2_UPDATES=0"; UPD_EXTRA=""; UPD_LIBS=""
case "$target" in
    starttest) SRCS="$WIN_CORE $X/husk-tl-x64.c $X/x64-starttest.c"; EXE="$OUT/rfl-x64-starttest.exe" ;;
    game) SRCS="$WIN_CORE $WIN_GAME $X/husk-tl-x64.c"; EXE="$OUT/$APP_NAME x64.exe"
        UPD_DEFS="-DPVZ2_UPDATES=1 -DPVZ2_UPDATE_FEED_URL=\"$FEED\" -DPVZ2_UPDATE_PUBLIC_KEY=\"$KEY\""
        UPD_EXTRA="-I$WS/include"; UPD_LIBS="-L$WS/x64 -lWinSparkle"
        LDX="-mwindows -lwinmm -lshell32 -lole32 -luuid -lcomctl32 -lwinhttp -lgdiplus -ldwmapi -luxtheme -lshlwapi -lcomdlg32" ;;
    game-portable) SRCS="$WIN_CORE $WIN_GAME $X/husk-tl-x64.c"; EXE="$OUT-portable/$APP_NAME x64.exe"
        LDX="-mwindows -lwinmm -lshell32 -lole32 -luuid -lcomctl32 -lwinhttp -lgdiplus -ldwmapi -luxtheme -lshlwapi -lcomdlg32" ;;
    *) echo "unknown target $target" >&2; exit 2 ;;
esac
UPD_OBJ="$OBJ/portable"
[ "$target" = game ] && UPD_OBJ="$OBJ/installed"
mkdir -p "$UPD_OBJ" "$(dirname "$EXE")"
OBJS=""
for f in $SRCS; do
    case "$f" in
        */pvz2-windows.c|*/pvz2-windows-updates.c)
            o="$UPD_OBJ/$(basename "$f" | sed 's/\.c$//').o"
            flags="-DPVZ2_APP_VERSION=\"$VERSION\" $UPD_DEFS $UPD_EXTRA" ;;
        *)  o="$OBJ/$(basename "$f" | sed 's/\.c$//').o"
            flags="-DPVZ2_APP_VERSION=\"$VERSION\"" ;;
    esac
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$PROFILE_DIR/pvz2-profile-generated.h" -nt "$o" ] || [ "$R/tools/pvz2-windows-settings.h" -nt "$o" ] || [ "$R/tools/pvz2-windows-updates.h" -nt "$o" ] || [ "$R/tools/pvz2-windows-identity.h" -nt "$o" ] || [ "$H/compat/win-compat.h" -nt "$o" ] || [ "$X/sigs-tables.inc" -nt "$o" ]; then
        echo "  cc $(basename "$f")"
        $CC $CFLAGS $flags -c "$f" -o "$o"
    fi
    OBJS="$OBJS $o"
done
if [ "$target" = game ] || [ "$target" = game-portable ]; then
    RC="$OBJ/win-manifest.o"
    if [ ! -f "$RC" ] || [ "$H/win-manifest.rc" -nt "$RC" ] || [ "$H/win-manifest.xml" -nt "$RC" ] || [ "$H/pvz2.ico" -nt "$RC" ]; then
        echo "  windres win-manifest.rc"
        $WINDRES -I"$H" -O coff -i "$H/win-manifest.rc" -o "$RC"
    fi
    OBJS="$OBJS $RC"
    # The dark mode library and its one C++ shim (runtime/win/build-darkmode.sh).
    "$H/build-darkmode.sh" "$CXX" "$AR" "$OUT"
    DM="$OBJ/pvz2-windows-darkmode.o"
    DMSRC="$R/tools/pvz2-windows-darkmode.cpp"
    if [ ! -f "$DM" ] || [ "$DMSRC" -nt "$DM" ] || [ "$R/tools/pvz2-windows-darkmode.h" -nt "$DM" ] || [ "$PROFILE_DIR/pvz2-profile-generated.h" -nt "$DM" ]; then
        echo "  c++ pvz2-windows-darkmode.cpp"
        $CXX -std=c++20 -O1 -g -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
            -I"$R/third_party/darkmodelib/include" -I"$N" -I"$PROFILE_DIR" -I"$R/tools" -c "$DMSRC" -o "$DM"
    fi
    OBJS="$OBJS $DM $OUT/libdarkmode.a"
fi
L="$SNX/build/lib"
$CXX $CXX_RT -o "$EXE" $OBJS -Wl,--start-group "$L/recompiler-library/libscarletnx-recompiler.a" "$L/llvm-backend/libsnx_llvm.a" \
    "$L/src/common/libscarletnx-common.a" "$L/llvm-backend/arm-backend/libsnx_arm_llvm.a" "$L/llvm-backend/arm-backend/libsnx_arm_rt.a" \
    "$L/llvm-backend/arm-backend/libsnx_arm_decode.a" "$L/libsnx_yyjson.a" "$L/libsnx_libdeflate.a" -Wl,--end-group \
    $LDX $UPD_LIBS -L"$OUT" -lz -lz-guest "$LLVM_LINK" -lpsapi -lws2_32 -lbcrypt -lsynchronization -lpthread -Wl,--error-limit=0
echo "built $EXE"
