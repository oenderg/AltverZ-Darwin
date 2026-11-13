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
#
# On a Mac or Linux it cross-builds for Windows when X64_CROSS_SYSROOT and LLVM_MINGW are set
# (runtime/win/x64/cross/setup-sysroot.sh makes the sysroot); the result is the same libraries.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:?usage: build-scarletnx.sh <folder>}"
PIN=f5c2fa84d52f75744739c3ad8edef79217f5a42c
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

# Cross-built on a Mac or Linux (X64_CROSS_SYSROOT names the folder runtime/win/x64/cross/setup-sysroot.sh
# made, LLVM_MINGW the llvm-mingw folder): the compiler is llvm-mingw's x86-64 one, and LLVM's headers and
# libraries for Windows come from the sysroot's CMake package. ScarletNX's CMake builds its one helper that
# runs during the build (snx-rtlib-gen) for the machine it is on, with that machine's clang.
if [ -n "${X64_CROSS_SYSROOT:-}" ]; then
    MB="${LLVM_MINGW:?set LLVM_MINGW to the llvm-mingw folder (its bin/ has x86_64-w64-mingw32-clang)}/bin"
    TOOLCHAIN_ARGS="-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64
        -DCMAKE_C_COMPILER=$MB/x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=$MB/x86_64-w64-mingw32-clang++
        -DCMAKE_RC_COMPILER=$MB/x86_64-w64-mingw32-windres
        -DLLVM_DIR=$X64_CROSS_SYSROOT/clang64/lib/cmake/llvm"
    export PATH="$MB:$PATH"
else
    TOOLCHAIN_ARGS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
fi
# shellcheck disable=SC2086
cmake -S "$DIR" -B "$DIR/build/lib" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release $TOOLCHAIN_ARGS \
    -DSNX_TOOLS_ONLY=ON -DBUILD_TESTING=OFF -DSNX_BUILD_TESTING=OFF \
    -DSNX_BUILD_NX=OFF -DSNX_BUILD_GUI=OFF -DSNX_BUILD_ANDROID=OFF -DSNX_BUILD_3DS=OFF \
    -DSNX_BUILD_VITA=OFF -DSNX_BUILD_AGB=OFF -DSNX_BUILD_GNUABI=OFF
cmake --build "$DIR/build/lib" --target scarletnx-recompiler
echo "build-scarletnx: $PIN with scarletnx.patch in $DIR/build/lib"
