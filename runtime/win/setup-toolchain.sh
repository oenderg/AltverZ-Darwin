#!/bin/sh
# Set up the Windows ARM64 toolchain in $TL_WIN_TOOLCHAIN (an empty folder of your choice):
#   llvm-mingw (clang + lld + the UCRT headers and libraries for Windows on ARM),
#   zlib for the runtime (win-arm64/lib/libz.a), and zlib for the guest
#   (win-arm64/lib/libz-guest.a: uLong is 64 bits there, so z_stream has Android's layout,
#   and every symbol carries a z_ prefix so the two never meet).
set -e
TC="${TL_WIN_TOOLCHAIN:?set TL_WIN_TOOLCHAIN to an empty folder for the toolchain}"
LLVM_MINGW=20261020
ZLIB=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
mkdir -p "$TC" && cd "$TC"
if [ ! -d "llvm-mingw-$LLVM_MINGW-ucrt-macos-universal" ]; then
    curl -fL -o llvm-mingw.tar.xz "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/llvm-mingw-$LLVM_MINGW-ucrt-macos-universal.tar.xz"
    tar xf llvm-mingw.tar.xz && rm llvm-mingw.tar.xz
fi
CC="$TC/llvm-mingw-$LLVM_MINGW-ucrt-macos-universal/bin/aarch64-w64-mingw32-clang"
AR="$TC/llvm-mingw-$LLVM_MINGW-ucrt-macos-universal/bin/llvm-ar"
if [ ! -d "zlib-$ZLIB" ]; then
    curl -fL -o zlib.tar.gz "https://github.com/madler/zlib/releases/download/v$ZLIB/zlib-$ZLIB.tar.gz"
    echo "$ZLIB_SHA256  zlib.tar.gz" | shasum -a 256 -c -
    tar xzf zlib.tar.gz && rm zlib.tar.gz
fi
SRCS="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil"
mkdir -p win-arm64/lib win-arm64/include/zlib-guest zlib-obj/host zlib-obj/guest
for f in $SRCS; do "$CC" -O2 -c "zlib-$ZLIB/$f.c" -o "zlib-obj/host/$f.o"; done
"$AR" rcs win-arm64/lib/libz.a zlib-obj/host/*.o
cp "zlib-$ZLIB/zlib.h" "zlib-$ZLIB/zconf.h" win-arm64/include/
# the guest's: a copy with 64-bit uLong
[ -d zlib-guest ] || cp -R "zlib-$ZLIB" zlib-guest
sed -i '' 's/^typedef unsigned long  uLong; .*/typedef unsigned long long uLong; \/* Android LP64: the guest z_stream layout *\//; s/^#  define z_off_t long$/#  define z_off_t long long/' zlib-guest/zconf.h
sed -i '' 's/^unsigned long ZEXPORT crc32_z(unsigned long crc,/uLong ZEXPORT crc32_z(uLong crc,/; s/^unsigned long ZEXPORT crc32(unsigned long crc,/uLong ZEXPORT crc32(uLong crc,/' zlib-guest/crc32.c
for f in $SRCS; do "$CC" -O2 -DZ_PREFIX -Dz_errmsg=tl_guestz_errmsg -c "zlib-guest/$f.c" -o "zlib-obj/guest/$f.o"; done
"$AR" rcs win-arm64/lib/libz-guest.a zlib-obj/guest/*.o
cp zlib-guest/zlib.h zlib-guest/zconf.h win-arm64/include/zlib-guest/
echo "toolchain ready in $TC"
