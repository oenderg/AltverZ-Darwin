#!/bin/sh
# Make the portable Windows zips: the program, ANGLE, the game's APK and CDN archive,
# a "portable" file (everything the game writes stays in data\ beside the .exe) and (for
# x86-64) the compiler's runtime DLLs. No updater: the zip's program has no WinSparkle.
#
#   runtime/win/package-zip.sh --arch all --apk "PvZ2 Reflourished 1.4.2.apk"
#
# Options:
#   --arch arm64|x64|all   which zips (default all)
#   --apk FILE             the game's APK, put in Game/ (required)
#   --tar FILE             the CDN archive, put in Game/1.4.2.main.tar (optional)
#   --angle DIR            libEGL.dll and libGLESv2.dll (and d3dcompiler_47.dll)
#                          to ship; arm64 defaults to prebuilt/win-arm64, x64 to
#                          prebuilt/win-x64 (runtime/win/build-angle.cmd makes them)
#   --exe-arm64 FILE       default out/win-portable/PvZ2 Reflourished.exe
#   --exe-x64 FILE         default out/win-x64-portable/PvZ2 Reflourished x64.exe
#   --x64-runtime DIR      extra DLLs the x64 program imports (libLLVM-23.dll,
#                          libc++.dll, ...); every .dll in it is copied
#   --out DIR              where the zips land (default out/zips)
#
# The zip programs come from the game-portable targets (build-win.sh game-portable,
# x64/build-x64.sh game-portable): the same game with no updater. The installed build,
# with WinSparkle, is what runtime/win/installer/build-installer.sh stages.
#
# The ARM64 program is linked statically (no runtime DLLs); the x64 program
# needs libLLVM-23.dll and libwinpthread-1.dll unless it was linked with a
# static LLVM. Those come from MSYS2's clang64, or from llvm-mingw's own
# runtime for libc++/libwinpthread/libunwind.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
VERSION="$(cat "$REPO/VERSION" 2>/dev/null || echo dev)"
ARCH=all
APK=""; TAR=""; OUT="$REPO/out/zips"
ANGLE_ARM="$REPO/prebuilt/win-arm64"
ANGLE_X64="$REPO/prebuilt/win-x64"
ANGLE_GIVEN=""
EXE_ARM="$REPO/out/win-portable/PvZ2 Reflourished.exe"
EXE_X64="$REPO/out/win-x64-portable/PvZ2 Reflourished x64.exe"
X64_RUNTIME=""

while [ $# -gt 0 ]; do
    case "$1" in
    --arch) ARCH="$2"; shift 2 ;;
    --apk) APK="$2"; shift 2 ;;
    --tar) TAR="$2"; shift 2 ;;
    --angle) ANGLE_ARM="$2"; ANGLE_X64="$2"; ANGLE_GIVEN=1; shift 2 ;;
    --exe-arm64) EXE_ARM="$2"; shift 2 ;;
    --exe-x64) EXE_X64="$2"; shift 2 ;;
    --x64-runtime) X64_RUNTIME="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

# Our ANGLE build is not in git: fetch-angle.sh downloads and checks it (runtime/win/ANGLE_ZIPS).
if [ -z "$ANGLE_GIVEN" ]; then "$H/fetch-angle.sh" "$ARCH" || exit 1; fi

[ -n "$APK" ] || { echo "package-zip: --apk is required" >&2; exit 2; }
[ -f "$APK" ] || { echo "package-zip: no APK at $APK" >&2; exit 2; }

GAME_VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$REPO/data/game-1.4.2.json")"
# The same pattern as the Apple files: PvZ2-Reflourished-<game>-Darwin-<version>-<platform>.
stamp="PvZ2-Reflourished-$GAME_VERSION-Darwin-$VERSION"
work="$REPO/out/zip-work"
mkdir -p "$OUT"

stage_common() {
    dir="$1"
    rm -rf "$dir"
    mkdir -p "$dir/Game"
    cp "$APK" "$dir/Game/pvz2.apk"
    # The portable build: the program keeps the saves, the settings, the game data and
    # the cache in data\ beside itself, and deleting the folder removes all of it.
    : > "$dir/portable"
    # The level key labels: the game's own level UI font (the one file the
    # project ships from the game's assets, as the Mac and iPad apps do).
    [ -f "$REPO/runtime/tools/fonts/level-ui.ttf" ] && cp "$REPO/runtime/tools/fonts/level-ui.ttf" "$dir/"
    if [ -n "$TAR" ]; then
        [ -f "$TAR" ] || { echo "package-zip: no CDN archive at $TAR" >&2; exit 2; }
        cp "$TAR" "$dir/Game/1.4.2.main.tar"
    fi
    cat > "$dir/README.txt" <<'EOF'
PvZ2: Reflourished for Windows (portable)
==========================================

Run "PvZ2 Reflourished.exe". It needs libEGL.dll and libGLESv2.dll beside it
(ANGLE) and the game in the Game folder.

This is the portable build: the file named "portable" beside the program makes
the game keep everything in the folder "data" that it makes here - the saves,
the settings and the keys, the OBB, the translation cache and the log. Nothing
is written to %APPDATA% or %LOCALAPPDATA%. To remove the game completely,
delete this whole folder, data and all.

This build has no automatic updater. "Check for Updates..." in the window menu
opens the releases page on GitHub in your browser. Installing a newer zip over
this folder keeps data, so the saves stay.

The first start on a 64-bit Intel/AMD PC translates the game, which takes a few
minutes; a small window shows how far it is. Later starts use the translation
that is kept in data; another PC translates once for itself.

The Settings window (window menu, or Ctrl+,) changes the keys and the window.
"In a level the key that works each control is shown on it" is on by default
(Settings, Keyboard); level-ui.ttf beside the program draws those labels.
ANGLE, which draws the game through Direct3D 11, is licensed as LICENSE-ANGLE.txt says.

The first start downloads the game's data (the OBB, about 1.3 GB) once,
with a small progress window and a Cancel button; it is checked before the
game starts. The archive in Game is the offline fallback for the live
content.
EOF
}

# The licenses folder: our GPL, what both programs build in, and (x64) ScarletNX and the DLLs
# beside it (runtime/win/licenses/README.txt lists them and where their sources are).
stage_licenses() {
    lic="$1/licenses"
    mkdir -p "$lic"
    cp "$REPO/LICENSE" "$lic/PvZ2-Reflourished.txt"
    cp "$REPO/runtime/win/licenses/README.txt" "$REPO/runtime/win/licenses/"*.txt "$lic/"
    if [ "$2" = x64 ]; then cp "$REPO/runtime/win/licenses/x64/"*.txt "$lic/"; fi
}

make_zip() {
    dir="$1"; name="$2"
    # zip updates an existing archive instead of replacing it, so a file that
    # left the staging folder (the old Vulkan DLLs) would stay in the zip.
    rm -f "$OUT/$name.zip"
    (cd "$dir" && zip -q -r -9 "$OUT/$name.zip" .)
    echo "made $OUT/$name.zip"
}

if [ "$ARCH" = arm64 ] || [ "$ARCH" = all ]; then
    dir="$work/arm64"
    stage_common "$dir"
    [ -f "$EXE_ARM" ] || { echo "package-zip: no portable ARM64 program at $EXE_ARM (runtime/win/build-win.sh game-portable)" >&2; exit 2; }
    cp "$EXE_ARM" "$dir/PvZ2 Reflourished.exe"
    [ -f "$ANGLE_ARM/libEGL.dll" ] && [ -f "$ANGLE_ARM/libGLESv2.dll" ] || {
        echo "package-zip: no libEGL.dll and libGLESv2.dll in $ANGLE_ARM (runtime/win/build-angle.cmd makes them, or pass --angle)" >&2; exit 2; }
    cp "$ANGLE_ARM"/*.dll "$dir/" 2>/dev/null || true
    [ -f "$ANGLE_ARM/LICENSE" ] && cp "$ANGLE_ARM/LICENSE" "$dir/LICENSE-ANGLE.txt" || true
    stage_licenses "$dir" arm64
    make_zip "$dir" "$stamp-Windows-ARM64"
fi

if [ "$ARCH" = x64 ] || [ "$ARCH" = all ]; then
    dir="$work/x64"
    stage_common "$dir"
    [ -f "$EXE_X64" ] || { echo "package-zip: no portable x64 program at $EXE_X64 (runtime/win/x64/build-x64.sh game-portable)" >&2; exit 2; }
    cp "$EXE_X64" "$dir/PvZ2 Reflourished x64.exe"
    [ -f "$ANGLE_X64/libEGL.dll" ] && [ -f "$ANGLE_X64/libGLESv2.dll" ] || {
        echo "package-zip: no libEGL.dll and libGLESv2.dll in $ANGLE_X64 (runtime/win/build-angle.cmd makes them, or pass --angle)" >&2; exit 2; }
    cp "$ANGLE_X64"/*.dll "$dir/" 2>/dev/null || true
    [ -f "$ANGLE_X64/LICENSE" ] && cp "$ANGLE_X64/LICENSE" "$dir/LICENSE-ANGLE.txt" || true
    if [ -n "$X64_RUNTIME" ]; then cp "$X64_RUNTIME"/*.dll "$dir/" 2>/dev/null || true; fi
    stage_licenses "$dir" x64
    make_zip "$dir" "$stamp-Windows-x64"
fi

echo "zips are in $OUT"
