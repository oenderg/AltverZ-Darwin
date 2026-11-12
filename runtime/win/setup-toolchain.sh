#!/bin/sh
# Set up the Windows ARM64 toolchain in $TL_WIN_TOOLCHAIN (an empty folder of your choice):
#   llvm-mingw (clang + lld + the UCRT headers and libraries for Windows on ARM),
#   zlib for the runtime (win-arm64/lib/libz.a), and zlib for the guest
#   (win-arm64/lib/libz-guest.a: uLong is 64 bits there, so z_stream has Android's layout,
#   and every symbol carries a z_ prefix so the two never meet).
set -e
H="$(cd "$(dirname "$0")" && pwd)"
TC="${TL_WIN_TOOLCHAIN:?set TL_WIN_TOOLCHAIN to an empty folder for the toolchain}"
LLVM_MINGW=20261020
LLVM_MINGW_SHA256=e9c13e4ede38e715003ffc674d4d00a1088df8235439d1f0c16188ff0704c17e
ZLIB=1.3.1
mkdir -p "$TC" && cd "$TC"
if [ ! -d "llvm-mingw-$LLVM_MINGW-ucrt-macos-universal" ]; then
    curl -fL --retry 3 -o llvm-mingw.tar.xz "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/llvm-mingw-$LLVM_MINGW-ucrt-macos-universal.tar.xz"
    echo "$LLVM_MINGW_SHA256  llvm-mingw.tar.xz" | shasum -a 256 -c -
    tar xf llvm-mingw.tar.xz && rm llvm-mingw.tar.xz
fi
CC="$TC/llvm-mingw-$LLVM_MINGW-ucrt-macos-universal/bin/aarch64-w64-mingw32-clang"
AR="$TC/llvm-mingw-$LLVM_MINGW-ucrt-macos-universal/bin/llvm-ar"
# zlib-$ZLIB and its guest copy (64-bit uLong), shared with the x86-64 build
"$H/fetch-zlib.sh" "$TC"
SRCS="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil"
mkdir -p win-arm64/lib win-arm64/include/zlib-guest zlib-obj/host zlib-obj/guest
for f in $SRCS; do "$CC" -O2 -c "zlib-$ZLIB/$f.c" -o "zlib-obj/host/$f.o"; done
"$AR" rcs win-arm64/lib/libz.a zlib-obj/host/*.o
cp "zlib-$ZLIB/zlib.h" "zlib-$ZLIB/zconf.h" win-arm64/include/
for f in $SRCS; do "$CC" -O2 -DZ_PREFIX -Dz_errmsg=tl_guestz_errmsg -c "zlib-guest/$f.c" -o "zlib-obj/guest/$f.o"; done
"$AR" rcs win-arm64/lib/libz-guest.a zlib-obj/guest/*.o
cp zlib-guest/zlib.h zlib-guest/zconf.h win-arm64/include/zlib-guest/
echo "toolchain ready in $TC"
