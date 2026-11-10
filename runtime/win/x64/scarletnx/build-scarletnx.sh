#!/bin/sh
# Fetch ScarletNX at the pinned commit, apply scarletnx.patch and build its recompiler
# library, in MSYS2's CLANG64 shell:
#
#   runtime/win/x64/scarletnx/build-scarletnx.sh <folder>
#
# <folder> becomes the checkout (a nested repository with only that commit); the libraries
# land in <folder>/build/lib, where runtime/win/x64/build-x64.sh links them from. Run again,
# it keeps the checkout and lets the build rebuild only what changed. The configuration is the
# one the release builds were made with: Release, the tools-only library, no tests, no
# front ends.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:?usage: build-scarletnx.sh <folder>}"
PIN=5b9c0fab49adc6d61d7a6abf39d9fbaf7b544f61
URL=https://github.com/scarletnx/ScarletNX.git
PATCH="$H/scarletnx.patch"

if [ ! -d "$DIR/.git" ] || [ "$(git -C "$DIR" rev-parse HEAD 2>/dev/null)" != "$PIN" ]; then
    rm -rf "$DIR"
    mkdir -p "$DIR"
    git -C "$DIR" init -q
    git -C "$DIR" remote add origin "$URL"
    git -C "$DIR" fetch -q --depth 1 origin "$PIN"
    git -C "$DIR" checkout -q FETCH_HEAD
fi
got="$(git -C "$DIR" rev-parse HEAD)"
[ "$got" = "$PIN" ] || { echo "build-scarletnx: HEAD is $got, expected $PIN" >&2; exit 1; }
if git -C "$DIR" apply --check "$PATCH" 2>/dev/null; then
    git -C "$DIR" apply "$PATCH"
elif ! git -C "$DIR" apply --check --reverse "$PATCH" 2>/dev/null; then
    echo "build-scarletnx: $PATCH does not apply to $PIN" >&2
    exit 1
fi

cmake -S "$DIR" -B "$DIR/build/lib" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DSNX_TOOLS_ONLY=ON -DBUILD_TESTING=OFF -DSNX_BUILD_TESTING=OFF \
    -DSNX_BUILD_NX=OFF -DSNX_BUILD_GUI=OFF -DSNX_BUILD_ANDROID=OFF -DSNX_BUILD_3DS=OFF \
    -DSNX_BUILD_VITA=OFF -DSNX_BUILD_AGB=OFF -DSNX_BUILD_GNUABI=OFF
cmake --build "$DIR/build/lib" --target scarletnx-recompiler
echo "build-scarletnx: $PIN with scarletnx.patch in $DIR/build/lib"
