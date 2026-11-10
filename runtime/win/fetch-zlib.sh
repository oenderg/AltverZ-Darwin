#!/bin/sh
# Put zlib's sources in <folder> for the Windows builds, twice:
#
#   runtime/win/fetch-zlib.sh <folder>
#
#   zlib-1.3.1/      zlib as it is, for the runtime
#   zlib-guest/      a copy for the guest: uLong is 64 bits there, so z_stream has Android's
#                    layout (the build adds Z_PREFIX, so the two never meet)
#   include/         zlib.h and zconf.h, and include/zlib-guest/ with the guest's
#
# setup-toolchain.sh (ARM64, on a Mac) and the x86-64 build (MSYS2) both use it. Plain POSIX
# sed, so it runs with BSD and GNU sed alike.
set -e
DIR="${1:?usage: fetch-zlib.sh <folder>}"
ZLIB=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
mkdir -p "$DIR"
cd "$DIR"
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
if [ ! -d "zlib-$ZLIB" ]; then
    curl -fL --retry 3 -o zlib.tar.gz "https://github.com/madler/zlib/releases/download/v$ZLIB/zlib-$ZLIB.tar.gz"
    got="$(sha256 zlib.tar.gz)"
    [ "$got" = "$ZLIB_SHA256" ] || { echo "fetch-zlib: zlib.tar.gz has sha256 $got, expected $ZLIB_SHA256" >&2; rm -f zlib.tar.gz; exit 1; }
    tar xzf zlib.tar.gz && rm zlib.tar.gz
fi
# The guest's copy, with 64-bit uLong and z_off_t. Each edit matches only the original line,
# so running this again changes nothing.
[ -d zlib-guest ] || cp -R "zlib-$ZLIB" zlib-guest
edit() {
    sed "$1" "$2" > "$2.new" && mv "$2.new" "$2"
}
edit 's/^typedef unsigned long  uLong; .*/typedef unsigned long long uLong; \/* Android LP64: the guest z_stream layout *\//; s/^#  define z_off_t long$/#  define z_off_t long long/' zlib-guest/zconf.h
edit 's/^unsigned long ZEXPORT crc32_z(unsigned long crc,/uLong ZEXPORT crc32_z(uLong crc,/; s/^unsigned long ZEXPORT crc32(unsigned long crc,/uLong ZEXPORT crc32(uLong crc,/' zlib-guest/crc32.c
mkdir -p include/zlib-guest
cp "zlib-$ZLIB/zlib.h" "zlib-$ZLIB/zconf.h" include/
cp zlib-guest/zlib.h zlib-guest/zconf.h include/zlib-guest/
echo "fetch-zlib: zlib $ZLIB in $DIR"
