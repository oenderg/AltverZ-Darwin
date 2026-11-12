#!/bin/sh
# Compile one runtime file for Windows ARM64 the way the Windows build does: cc.sh <file> [-fsyntax-only|-c -o out.o]
H="$(cd "$(dirname "$0")" && pwd)"
TC="${TL_WIN_TOOLCHAIN:?set TL_WIN_TOOLCHAIN to the folder holding llvm-mingw-* and win-arm64 (see runtime/win/README.md)}"
f="$1"; shift
exec "$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/aarch64-w64-mingw32-clang" -std=gnu11 -g -O1 \
    -D_GNU_SOURCE -DTL_WINDOWS=1 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wno-unused-function \
    -include "$H/compat/win-compat.h" -I"$H/compat" -I"$H/../src/translation-layer" -I"$H/../src/translation-layer-next" \
    ${PROFILE_DIR:+-I"$PROFILE_DIR"} -I"$H/../tools" -I"$TC/win-arm64/include" "$f" "$@"
