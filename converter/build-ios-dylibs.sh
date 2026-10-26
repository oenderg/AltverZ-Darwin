#!/bin/bash
# Convert the guest libraries for a real device: each .so becomes a signed arm64 Mach-O
# dylib for iOS, with every load-time rewrite baked in (scripts/aot-convert.py).
#
#   build-ios-dylibs.sh [apk|lib-dir] [out-dir]
#
# This is the device half of the pipeline. The conversion itself is target-agnostic, but
# the result is not: a dylib linked against the macOS SDK will not load on iOS, so the
# device build has to be done again from the same .so files with an iOS target. Everything
# expensive -- the ELF analysis -- is the same work either way.
#
# What comes out, in the order the linker needs them:
#   libbionic-shim.dylib     bionic, OpenSLES, OpenGL ES and the vendor runtimes the guest
#                            imports and iOS does not have
#   libc++_shared.dylib      the C++ runtime the game brings with it (not iOS's)
#   libNimble.dylib          the game's JNI helper, which libPVZ2 calls by name
#   libPVZ2.dylib            the game
#   <mod>.dylib              the mod (libRFLib.dylib or libSnowie.dylib)
# plus a .facts file beside each dylib: the symbol map the runtime needs, because the
# guest's names live inside the image as bytes rather than in a symbol table.
#
# The mod's inline hooks are baked into libPVZ2 as well. A device's code is signed
# and read-only, so the branches the mod would write at start-up cannot be written there;
# they are written here instead, into libPVZ2's own text, before the signature. Set
# HOOKS=<the frozen hook table> to bake them; without it the build has no hooks and the
# game waits for ones that never arrive.
set -euo pipefail

R="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-}"
OUT="${2:-$R/../research/build/ios-dylibs}"
HOOKS="${HOOKS:-}"
TARGET="${TARGET:-arm64-apple-ios15.5}"
SDKNAME="${SDKNAME:-iphoneos}"
SDK="$(xcrun --sdk "$SDKNAME" --show-sdk-path)"
CC=(xcrun --sdk "$SDKNAME" clang++)
CFLAGS=(-target "$TARGET" -isysroot "$SDK" -arch arm64 -O2)

if [ -z "$SRC" ]; then
    echo "usage: build-ios-dylibs.sh <apk|lib-dir> [out-dir]" >&2
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
MOD_LIBRARY="${MOD_LIBRARY:-libRFLib}"; MOD_LIBRARY="${MOD_LIBRARY%.so}"
for so in libc++_shared libNimble libPVZ2 "$MOD_LIBRARY"; do
    [ -f "$LIB/$so.so" ] || { echo "no $so.so in $LIB" >&2; exit 1; }
done

echo "==> building the shim for $TARGET"
# The hand-written half first, on its own, so the generator knows which names to leave to
# it: two definitions of one symbol is a link error, not a merge.
"${CC[@]}" "${CFLAGS[@]}" -dynamiclib -o "$WORK/libbionic-base.dylib" "$R/bionic-shim.c" \
    -install_name @rpath/libbionic-base.dylib
xcrun nm -g "$WORK/libbionic-base.dylib" | awk '$2 ~ /^[TDRB]$/ {print $3}' |
    sed 's/^_//' | sort -u > "$WORK/handwritten.txt"
echo "    $(wc -l < "$WORK/handwritten.txt" | tr -d ' ') hand-written exports"

python3 "$R/aot-mkshim.py" "$LIB"/*.so -o "$WORK/shim-generated.c" \
    --exclude-file "$WORK/handwritten.txt" --target "$TARGET" \
    --forward-gl --forward-opensles --link=-lz \
    --record "$WORK/shim-split.json" | tail -1

"${CC[@]}" "${CFLAGS[@]}" -dynamiclib -o "$OUT/libbionic-shim.dylib" \
    "$R/bionic-shim.c" "$WORK/shim-generated.c" \
    -install_name @rpath/libbionic-shim.dylib
# --force, because the converter has already signed what it produced and codesign
# refuses to sign a file that carries a signature ("is already signed", exit 1). The
# build runs under `set -e`, so that refusal would stop it half way through.
codesign --force -s - "$OUT/libbionic-shim.dylib"
echo "    $(xcrun nm -g "$OUT/libbionic-shim.dylib" | awk '$2 ~ /^[TDRB]$/' | wc -l | tr -d ' ') symbols exported"

# The order matters: a library links against the ones it imports from, so those have to
# be built first. libPVZ2 is the slow one (22 MB of code); it is worth saying so.
convert() {
    local so="$1" index="$2"; shift 2
    local dylib="$OUT/$so.dylib"
    [ -f "$dylib" ] && [ -z "${FORCE:-}" ] && { echo "==> $so.dylib is already there"; return; }
    echo "==> converting $so.so for $TARGET"
    set +e
    python3 "$R/aot-convert.py" "$LIB/$so.so" -o "$OUT" --target "$TARGET" \
        --lib-index "$index" \
        --shim "$OUT/libbionic-shim.dylib" --shim=-lz "$@" 2>&1 |
        grep -vE "^  ->" | sed 's/^/    /'
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" != 0 ]; then echo "    FAILED ($rc) -- no $so.dylib"; return 1; fi
    # --force for the same reason as the shim: the converter has already signed the
    # dylib it produced, and codesign refuses to sign a file that carries a signature.
    codesign --force -s - "$dylib"
}

# The index is which of the guest's libraries this is, in the order they are converted. It
# is baked into the stubs that stand in for the reserved register, so the order is part of
# the build: the shim keeps one thread-local word per library and the stub names its own.
convert libc++_shared 0
convert libNimble 1
# The mod's hooks are baked into libPVZ2, which is where all 290 sites are. The table is
# the frozen one (the mod's own, produced by scripts/aot-hooks.py from a codewrite log);
# HOOKS names it. Without it the device build would ship a game whose hooks are never
# applied, which is the state the Mac route was in before the bake.
HOOK_ARG=()
if [ -n "${HOOKS:-}" ]; then
    [ -f "$HOOKS" ] || { echo "no hook table at $HOOKS" >&2; exit 2; }
    echo "==> baking the mod's hooks from $HOOKS"
    HOOK_ARG=(--hooks "$HOOKS")
fi
convert libPVZ2 2 --shim "$OUT/libc++_shared.dylib" --shim "$OUT/libNimble.dylib" \
                  ${HOOK_ARG[@]+"${HOOK_ARG[@]}"}
convert "$MOD_LIBRARY" 3 --shim "$OUT/libPVZ2.dylib" --shim "$OUT/libc++_shared.dylib" \
                  --shim "$OUT/libNimble.dylib"

echo
echo "==> in $OUT:"
for d in "$OUT"/*.dylib; do printf "    %-24s %s\n" "$(basename "$d")" "$(du -h "$d" | cut -f1)"; done