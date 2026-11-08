#!/bin/sh
# Build the x86-64 Windows programs, in MSYS2's CLANG64 shell (clang, LLVM 23):
#   build-x64.sh <runtime folder> <ScarletNX checkout, its library built in build/lib> <zlib folder> <out folder> [starttest|game]
# <zlib folder> holds zlib-1.3.1 and zlib-guest (setup-toolchain.sh makes both) and include/ with their headers.
set -e
R="$(cd "$1" && pwd)"; SNX="$(cd "$2" && pwd)"; Z="$(cd "$3" && pwd)"; OUT="$4"; target="${5:-starttest}"
H="$R/win"; N="$R/src/translation-layer-next"; T="$R/src/translation-layer"; X="$H/x64"
OBJ="$OUT/obj"; mkdir -p "$OBJ" "$OUT"
CC=clang
CFLAGS="-std=gnu11 -g -O1 -D_GNU_SOURCE -DTL_WINDOWS=1 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wno-unused-function
  -include $H/compat/win-compat.h -I$H/compat -I$T -I$N -I$R/tools -I$Z/include -I$X -I$SNX/src"

# zlib twice: the runtime's, and the guest's (64-bit uLong, z_ prefixed)
ZS="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil"
if [ ! -f "$OUT/libz-guest.a" ]; then
    mkdir -p "$OBJ/zh" "$OBJ/zg"
    for f in $ZS; do $CC -O2 -c "$Z/zlib-1.3.1/$f.c" -o "$OBJ/zh/$f.o"; done
    for f in $ZS; do $CC -O2 -DZ_PREFIX -Dz_errmsg=tl_guestz_errmsg -c "$Z/zlib-guest/$f.c" -o "$OBJ/zg/$f.o"; done
    llvm-ar rcs "$OUT/libz.a" "$OBJ"/zh/*.o
    llvm-ar rcs "$OUT/libz-guest.a" "$OBJ"/zg/*.o
fi

. "$H/sources.sh"
case "$target" in
    starttest) SRCS="$WIN_CORE $X/husk-tl-x64.c $X/x64-starttest.c"; EXE="$OUT/rfl-x64-starttest.exe" ;;
    game) SRCS="$WIN_CORE $WIN_GAME $X/husk-tl-x64.c"; EXE="$OUT/PvZ2 Reflourished x64.exe"; LDX="-mwindows -lwinmm -lshell32 -lole32 -lcomctl32 -lwinhttp -lgdiplus" ;;
    *) echo "unknown target $target" >&2; exit 2 ;;
esac
OBJS=""
for f in $SRCS; do
    o="$OBJ/$(basename "$f" | sed 's/\.c$//').o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$H/compat/win-compat.h" -nt "$o" ] || [ "$X/sigs-tables.inc" -nt "$o" ]; then
        echo "  cc $(basename "$f")"
        $CC $CFLAGS -c "$f" -o "$o"
    fi
    OBJS="$OBJS $o"
done
if [ "$target" = game ]; then
    RC="$OBJ/win-manifest.o"
    if [ ! -f "$RC" ] || [ "$H/win-manifest.rc" -nt "$RC" ] || [ "$H/win-manifest.xml" -nt "$RC" ]; then
        echo "  windres win-manifest.rc"
        windres -I"$H" -O coff -i "$H/win-manifest.rc" -o "$RC"
    fi
    OBJS="$OBJS $RC"
fi
L="$SNX/build/lib"
clang++ -o "$EXE" $OBJS -Wl,--start-group "$L/recompiler-library/libscarletnx-recompiler.a" "$L/llvm-backend/libsnx_llvm.a" \
    "$L/src/common/libscarletnx-common.a" "$L/llvm-backend/arm-backend/libsnx_arm_llvm.a" "$L/llvm-backend/arm-backend/libsnx_arm_rt.a" \
    "$L/llvm-backend/arm-backend/libsnx_arm_decode.a" "$L/libsnx_yyjson.a" "$L/libsnx_libdeflate.a" -Wl,--end-group \
    $LDX -L"$OUT" -lz -lz-guest -lLLVM-23 -lpsapi -lws2_32 -lbcrypt -lsynchronization -lpthread -Wl,--error-limit=0
echo "built $EXE"
