#!/bin/sh
# Make the Windows zips that run without MSYS2: the program, ANGLE, the game's
# APK and CDN archive, and (for x86-64) the compiler's runtime DLLs.
#
#   runtime/win/package-zip.sh --arch all --apk "PvZ2 Reflourished 1.4.2.apk"
#
# Options:
#   --arch arm64|x64|all   which zips (default all)
#   --apk FILE             the game's APK, put in Game/ (required)
#   --tar FILE             the CDN archive, put in Game/1.4.2.main.tar (optional)
#   --angle DIR            libEGL.dll and libGLESv2.dll (and the D3D/Vulkan
#                          companions) to ship; arm64 defaults to
#                          out/win/angle-arm64, x64 to out/win/angle-x64
#   --x64-exe FILE         default out/win-x64/PvZ2 Reflourished x64.exe
#   --x64-runtime DIR      extra DLLs the x64 program imports (libLLVM-23.dll,
#                          libc++.dll, ...); every .dll in it is copied
#   --out DIR              where the zips land (default out/zips)
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
ANGLE_ARM="$REPO/out/win/angle-arm64"
ANGLE_X64="$REPO/out/win/angle-x64"
X64_EXE="$REPO/out/win-x64/PvZ2 Reflourished x64.exe"
X64_RUNTIME=""

while [ $# -gt 0 ]; do
    case "$1" in
    --arch) ARCH="$2"; shift 2 ;;
    --apk) APK="$2"; shift 2 ;;
    --tar) TAR="$2"; shift 2 ;;
    --angle) ANGLE_ARM="$2"; ANGLE_X64="$2"; shift 2 ;;
    --x64-exe) X64_EXE="$2"; shift 2 ;;
    --x64-runtime) X64_RUNTIME="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

[ -n "$APK" ] || { echo "package-zip: --apk is required" >&2; exit 2; }
[ -f "$APK" ] || { echo "package-zip: no APK at $APK" >&2; exit 2; }

stamp="PvZ2Reflourished-$VERSION"
work="$REPO/out/zip-work"
mkdir -p "$OUT"

stage_common() {
    dir="$1"
    rm -rf "$dir"
    mkdir -p "$dir/Game"
    cp "$APK" "$dir/Game/pvz2.apk"
    # The level key labels: the game's own level UI font (the one file the
    # project ships from the game's assets, as the Mac and iPad apps do).
    [ -f "$REPO/runtime/tools/fonts/level-ui.ttf" ] && cp "$REPO/runtime/tools/fonts/level-ui.ttf" "$dir/"
    if [ -n "$TAR" ]; then
        [ -f "$TAR" ] || { echo "package-zip: no CDN archive at $TAR" >&2; exit 2; }
        cp "$TAR" "$dir/Game/1.4.2.main.tar"
    fi
    cat > "$dir/README.txt" <<'EOF'
PvZ2: Reflourished for Windows
==============================

Run "PvZ2 Reflourished.exe". It needs libEGL.dll and libGLESv2.dll beside it
(ANGLE) and the game in the Game folder.

The first start on a 64-bit Intel/AMD PC translates the game, which takes a few
minutes; a small window shows how far it is. Later starts use the translation
that is kept in %LOCALAPPDATA%\PvZ2 Reflourished.

Where the game keeps things
  saves and settings   %APPDATA%\PvZ2 Reflourished
  game data, the OBB,
  the translation      %LOCALAPPDATA%\PvZ2 Reflourished
A file named "portable" beside the program puts everything in "data" beside it.
The Settings window (window menu, or Ctrl+,) changes the keys and the window.
"In a level the key that works each control is shown on it" is on by default
(Settings, Keyboard); level-ui.ttf beside the program draws those labels.

The first start downloads the game's data (the OBB, about 1.3 GB) once,
with a small progress window and a Cancel button; it is checked before the
game starts. The archive in Game is the offline fallback for the live
content.
EOF
}

make_zip() {
    dir="$1"; name="$2"
    (cd "$dir" && zip -q -r -9 "$OUT/$name.zip" .)
    echo "made $OUT/$name.zip"
}

if [ "$ARCH" = arm64 ] || [ "$ARCH" = all ]; then
    dir="$work/arm64"
    stage_common "$dir"
    cp "$REPO/out/win/PvZ2 Reflourished.exe" "$dir/PvZ2 Reflourished.exe"
    [ -f "$ANGLE_ARM/libEGL.dll" ] && [ -f "$ANGLE_ARM/libGLESv2.dll" ] || {
        echo "package-zip: put libEGL.dll and libGLESv2.dll in $ANGLE_ARM or pass --angle" >&2; exit 2; }
    cp "$ANGLE_ARM"/*.dll "$dir/" 2>/dev/null || true
    make_zip "$dir" "$stamp-windows-arm64"
fi

if [ "$ARCH" = x64 ] || [ "$ARCH" = all ]; then
    dir="$work/x64"
    stage_common "$dir"
    [ -f "$X64_EXE" ] || { echo "package-zip: no x64 program at $X64_EXE" >&2; exit 2; }
    cp "$X64_EXE" "$dir/PvZ2 Reflourished x64.exe"
    [ -f "$ANGLE_X64/libEGL.dll" ] && [ -f "$ANGLE_X64/libGLESv2.dll" ] || {
        echo "package-zip: put libEGL.dll and libGLESv2.dll in $ANGLE_X64 or pass --angle" >&2; exit 2; }
    cp "$ANGLE_X64"/*.dll "$dir/" 2>/dev/null || true
    if [ -n "$X64_RUNTIME" ]; then cp "$X64_RUNTIME"/*.dll "$dir/" 2>/dev/null || true; fi
    make_zip "$dir" "$stamp-windows-x64"
fi

echo "zips are in $OUT"
