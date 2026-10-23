#!/bin/bash
# Build the shim: the hand-written behaviour in scripts/bionic-shim.c, plus a generated
# placeholder for every import no system library provides.
#
# The two halves must not define the same symbol. The hand-written half is built on its
# own first and its exports are what the generator is told to leave alone -- asking one
# dylib to define `epoll_wait` twice is a link error, not a merge.
#
#   build-shim.sh [out-dir]
#
# The guest links against zlib as well as against this shim, so the generator's probe
# links against zlib too: a name zlib provides must not get a placeholder here, or the
# guest binds to the placeholder instead of to zlib and its inflate decompresses nothing.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
d="${1:-${OUT:-$R/../research/build/shim}}"
SHIM_SRC="${SHIM_SRC:-$R/bionic-shim.c}"
LIBS="${LIBS:-${1:-}}"
CONV="${CONV:-$R/aot-convert.py}"
MKSHIM="${MKSHIM:-$R/aot-mkshim.py}"
TARGET="${TARGET:-arm64-apple-macos}"

if [ -z "$LIBS" ] || [ ! -d "$LIBS" ]; then
    echo "usage: build-shim.sh <dir with the guest's .so files> [out-dir]" >&2
    exit 2
fi
if [ "$#" -ge 2 ]; then d="$2"; fi

mkdir -p "$d"

echo "==> building the hand-written half on its own, to find out what it exports"
xcrun clang++ -dynamiclib -arch arm64 -o "$d/libbionic-base.dylib" "$SHIM_SRC" \
    -install_name @rpath/libbionic-base.dylib
# the exports, with the Mach-O underscore stripped, are the names not to generate
xcrun nm -g "$d/libbionic-base.dylib" | awk '$2 ~ /^[TDRB]$/ {print $3}' |
    sed 's/^_//' | sort -u > "$d/handwritten.txt"
echo "    $(wc -l < "$d/handwritten.txt" | tr -d ' ') hand-written exports"

echo "==> generating placeholders for everything else the libraries import"
python3 "$MKSHIM" "$LIBS"/*.so -o "$d/shim-generated.c" \
    --exclude-file "$d/handwritten.txt" --forward-gl --forward-opensles --link=-lz \
    --record "$d/shim-split.json" | tail -1

echo "==> linking the shim"
xcrun clang++ -dynamiclib -arch arm64 -o "$d/libbionic-shim.dylib" \
    "$SHIM_SRC" "$d/shim-generated.c" -install_name @rpath/libbionic-shim.dylib
codesign -s - "$d/libbionic-shim.dylib"
echo "    $(xcrun nm -g "$d/libbionic-shim.dylib" | awk '$2 ~ /^[TDRB]$/' | wc -l | tr -d ' ') symbols exported"
