#!/bin/sh
# Build the x86-64 Windows program on a Mac or Linux, with no Windows and no MSYS2:
#
#   TL_WIN_TOOLCHAIN=<toolchain folder> runtime/win/build-x64-cross.sh [game-portable|game|starttest]
#
# It runs the same steps the Windows build runs in MSYS2's CLANG64 shell, with llvm-mingw's x86-64 compiler:
#   1. x64/cross/setup-sysroot.sh   the pinned MSYS2 packages: LLVM 23 for Windows and the DLLs the program ships with
#   2. fetch-zlib.sh                zlib for the runtime and for the guest
#   3. x64/scarletnx/build-scarletnx.sh   ScarletNX's recompiler library at the pinned commit, with scarletnx.patch
#   4. x64/build-x64.sh             the program (the installed one, game, also takes WinSparkle: fetch-winsparkle.sh)
#   5. x64/collect-dlls.sh          the DLLs it needs, for package-zip.sh and build-installer.sh (--x64-runtime)
#
# Needs llvm-mingw (runtime/win/setup-toolchain.sh unpacks it in $TL_WIN_TOOLCHAIN, or LLVM_MINGW names another
# copy, for a Linux host its Linux release), cmake, ninja, git, curl, zstd, python3 and a C compiler named clang
# (the build's one helper that runs on this machine). Keep the folders' paths free of spaces.
#
# Output: out/win-<profile>-x64 (game) or out/win-<profile>-x64-portable (game-portable), the DLLs in
# out/win-<profile>-x64-runtime, and the work (sysroot, zlib, ScarletNX) in out/win-x64-cross-work, which
# X64_CROSS_WORK moves. PROFILE, APP_VERSION, SPARKLE_FEED_URL and SPARKLE_PUBLIC_KEY work as in build-win.sh.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
PROFILE="${PROFILE:-altverz}"
. "$REPO/scripts/profile.sh"
profile_generate "$REPO/out/win-profile-$PROFILE"
export PROFILE_DIR

LLVM_MINGW="${LLVM_MINGW:-${TL_WIN_TOOLCHAIN:?set TL_WIN_TOOLCHAIN (runtime/win/setup-toolchain.sh) or LLVM_MINGW}/llvm-mingw-20261020-ucrt-macos-universal}"
[ -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" ] || {
    echo "build-x64-cross: no $LLVM_MINGW/bin/x86_64-w64-mingw32-clang; set LLVM_MINGW to an llvm-mingw folder" >&2; exit 1; }
WORK="${X64_CROSS_WORK:-$REPO/out/win-x64-cross-work}"
X64_CROSS_SYSROOT="${X64_CROSS_SYSROOT:-$WORK/sysroot}"
export LLVM_MINGW X64_CROSS_SYSROOT
target="${1:-game-portable}"
OUT="$REPO/out/win-$PROFILE-x64"

"$H/x64/cross/setup-sysroot.sh" "$X64_CROSS_SYSROOT"
"$H/fetch-zlib.sh" "$WORK/zlib"
"$H/x64/scarletnx/build-scarletnx.sh" "$WORK/scarletnx"
[ "$target" = game ] && "$H/fetch-winsparkle.sh"
sh "$H/x64/build-x64.sh" "$REPO/runtime" "$WORK/scarletnx" "$WORK/zlib" "$OUT" "$target"

case "$target" in
    game) EXE="$OUT/$PROFILE_APP_NAME x64.exe" ;;
    game-portable) EXE="$OUT-portable/$PROFILE_APP_NAME x64.exe" ;;
    *) exit 0 ;;
esac
CLANG64_BIN="$X64_CROSS_SYSROOT/clang64/bin" PATH="$LLVM_MINGW/bin:$PATH" \
    bash "$H/x64/collect-dlls.sh" "$EXE" "$OUT-runtime"
