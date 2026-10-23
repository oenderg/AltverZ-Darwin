#!/bin/bash
# Convert the guest libraries for the Mac: each .so becomes a signed arm64 Mach-O
# dylib, with every load-time rewrite baked in -- the thread-block references, the
# PC-relative pairs into data, the stubs for the reserved register -- and the mod's
# 290 inline hooks baked into libPVZ2.
#
#   scripts/build-macos-dylibs.sh [apk|lib-dir] [out-dir]
#
# This is the Mac half of the pipeline; the device half is scripts/build-ios-dylibs.sh
# and the two differ only in the target they link for. The conversion itself is
# target-agnostic, but a dylib linked against the macOS SDK will not load on iOS.
#
# What comes out:
#   libbionic-shim.dylib     bionic, OpenSLES, OpenGL ES and the vendor runtimes the
#                            guest imports and macOS does not have
#   libc++_shared.dylib      the C++ runtime the game brings with it (not macOS's)
#   libNimble.dylib          the game's JNI helper, which libPVZ2 calls by name
#   libPVZ2.dylib            the game
#   libRFLib.dylib           the game's framework layer
# plus a .facts file beside each dylib: the symbol map the runtime needs, because the
# guest's names live inside the image as bytes rather than in a symbol table.
#
# HOOKS names the frozen hook table. Without it the build ships a game whose 290 mod
# hooks are never applied, and the game waits for hooks that never arrive.
#
# GLES names ANGLE's libGLESv2: the guest's libraries are linked against it, so the
# guest's gl* calls reach the real GLES rather than a forwarding stub.
set -euo pipefail

R="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-}"
OUT="${2:-$R/../research/build/macos-dylibs}"
HOOKS="${HOOKS:-}"
TARGET="${TARGET:-arm64-apple-macos}"
SDKNAME="${SDKNAME:-macosx}"
SDK="$(xcrun --sdk "$SDKNAME" --show-sdk-path)"
CC=(xcrun --sdk "$SDKNAME" clang++)
CFLAGS=(-target "$TARGET" -isysroot "$SDK" -arch arm64 -O2)
GLES="${GLES:-$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Chromium Embedded Framework.framework/Versions/A/Libraries/libGLESv2.dylib}"

if [ -z "$SRC" ]; then
    echo "usage: build-macos-dylibs.sh <apk|lib-dir> [out-dir]" >&2
    exit 2
fi

WORK="$OUT/.work"
mkdir -p "$OUT" "$WORK"
if [ -f "$SRC" ]; then
    echo "==> taking the guest libraries out of $(basename "$SRC")"
    rm -rf "$WORK/libs"; mkdir -p "$WORK/libs"
    unzip -o -q "$SRC" 'lib/arm64-v8a/*.so' -d "$WORK/libs"
    LIB="$WORK/libs/lib/arm64-v8a"
else
    LIB="$SRC"
fi
for so in libc++_shared libNimble libPVZ2 libRFLib; do
    [ -f "$LIB/$so.so" ] || { echo "no $so.so in $LIB" >&2; exit 1; }
done

echo "==> building the shim for $TARGET"
# The hand-written half first, on its own, so the generator knows which names to leave
# to it: two definitions of one symbol is a link error, not a merge.
bash "$R/build-shim.sh" "$LIB" "$WORK/shim" | sed 's/^/    /'

echo "==> converting the guest libraries"
# The shim, and zlib: a name zlib provides must not get a placeholder in the shim, or
# the guest binds to the placeholder and its inflate decompresses nothing.
SHIM=(--shim "$WORK/shim/libbionic-shim.dylib" --shim=-lz)
[ -f "$GLES" ] && SHIM+=(--shim "$GLES")

convert() {
    local so="$1" index="$2"; shift 2
    local dylib="$OUT/$so.dylib"
    if [ -f "$dylib" ] && [ -z "${FORCE:-}" ]; then echo "==> $so.dylib is already there"; return; fi
    echo "==> converting $so.so"
    set +e
    python3 "$R/aot-convert.py" "$LIB/$so.so" -o "$OUT" --target "$TARGET" \
        --lib-index "$index" "${SHIM[@]}" "$@" 2>&1 |
        grep -vE "^  ->|word\(s\)|site\(s\)|^  self-check" | sed 's/^/    /'
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" != 0 ]; then echo "    FAILED ($rc) -- no $so.dylib"; return 1; fi
    # --force: the converter has already signed what it produced, and codesign refuses
    # to sign a file that carries a signature ("is already signed", exit 1).
    codesign --force -s - "$dylib"
}

# The index is which of the guest's libraries this is, in the order they are converted.
# It is baked into the stubs that stand in for the reserved register, so the order is
# part of the build: the shim keeps one thread-local word per library and the stub
# names its own.
convert libc++_shared 0
convert libNimble 1
HOOK_ARG=()
if [ -n "$HOOKS" ]; then
    [ -f "$HOOKS" ] || { echo "no hook table at $HOOKS" >&2; exit 2; }
    echo "==> baking the mod's hooks from $HOOKS"
    HOOK_ARG=(--hooks "$HOOKS")
fi
convert libPVZ2 2 --shim "$OUT/libc++_shared.dylib" --shim "$OUT/libNimble.dylib" \
                  ${HOOK_ARG[@]+"${HOOK_ARG[@]}"}
convert libRFLib 3 --shim "$OUT/libPVZ2.dylib" --shim "$OUT/libc++_shared.dylib" \
                  --shim "$OUT/libNimble.dylib"

cp "$WORK/shim/libbionic-shim.dylib" "$OUT/" && codesign --force -s - "$OUT/libbionic-shim.dylib"

echo
echo "==> in $OUT:"
for d in "$OUT"/*.dylib; do printf "    %-24s %s\n" "$(basename "$d")" "$(du -h "$d" | cut -f1)"; done
echo
echo "the .facts file beside each dylib is the symbol map the runtime needs;"
echo "the app bundle needs them too (tools/regress/build-macos-app.sh copies them)."
