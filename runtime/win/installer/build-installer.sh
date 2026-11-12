#!/bin/sh
# Stage the two architectures for the Windows setup program and (with --zip) the file to carry
# them to a Windows PC, where runtime\win\installer\build-installer.cmd runs Inno Setup on it.
#
#   runtime/win/installer/build-installer.sh --apk "<the game's APK>" [--tar FILE]
#
# Options:
#   --apk FILE         the game's APK, put in Game/ (required)
#   --tar FILE         the CDN archive, put in Game/<the CDN archive> (optional)
#   --version V        the setup program's version (default: VERSION)
#   --angle DIR        libEGL.dll and libGLESv2.dll for both architectures (default:
#                      prebuilt/win-arm64 and prebuilt/win-x64)
#   --exe-arm64 FILE   default out/win-<profile>/<app name>.exe (build-win.sh game)
#   --exe-x64 FILE     default out/win-<profile>-x64/<app name> x64.exe
#   --x64-runtime DIR  the DLLs the x64 program imports (default out/win/x64-runtime)
#   --out DIR          the staged tree and the zip (default out/win-installer)
#   --zip              also write stage.zip, for a Windows PC to download
#
# The staged ARM64 tree is the installed program (it links WinSparkle, so WinSparkle.dll goes
# beside it); the x64 tree is the same with its own DLLs. Neither carries a "portable" file:
# the installer must install the normal build, whose saves and settings live in %APPDATA%.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../../.." && pwd)"
PROFILE="${PROFILE:-altverz}"; ROOT="$REPO"
. "$REPO/scripts/profile.sh"
VERSION="${APP_VERSION:-$PROFILE_APP_VERSION}"
APK=""; TAR=""; OUT="$REPO/out/win-installer"
ANGLE="$REPO/prebuilt"
EXE_ARM="$REPO/out/win-$PROFILE/$PROFILE_APP_NAME.exe"
EXE_X64="$REPO/out/win-$PROFILE-x64/$PROFILE_APP_NAME x64.exe"
X64_RUNTIME="$REPO/out/win-$PROFILE/x64-runtime"
MAKE_ZIP=""

while [ $# -gt 0 ]; do
    case "$1" in
    --apk) APK="$2"; shift 2 ;;
    --tar) TAR="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --angle) ANGLE="$2"; shift 2 ;;
    --exe-arm64) EXE_ARM="$2"; shift 2 ;;
    --exe-x64) EXE_X64="$2"; shift 2 ;;
    --x64-runtime) X64_RUNTIME="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --zip) MAKE_ZIP=1; shift ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "build-installer: unknown option $1" >&2; exit 2 ;;
    esac
done

[ -n "$APK" ] || { echo "build-installer: --apk is required" >&2; exit 2; }
[ -f "$APK" ] || { echo "build-installer: no APK at $APK" >&2; exit 2; }
[ -f "$EXE_ARM" ] || { echo "build-installer: no installed ARM64 program at $EXE_ARM" >&2; exit 2; }
[ -f "$EXE_X64" ] || { echo "build-installer: no x64 program at $EXE_X64" >&2; exit 2; }
[ -f "$REPO/prebuilt/winsparkle/arm64/WinSparkle.dll" ] || {
    echo "build-installer: no prebuilt/winsparkle/arm64/WinSparkle.dll (runtime/win/fetch-winsparkle.sh)" >&2; exit 2; }
[ -f "$REPO/prebuilt/winsparkle/x64/WinSparkle.dll" ] || {
    echo "build-installer: no prebuilt/winsparkle/x64/WinSparkle.dll (runtime/win/fetch-winsparkle.sh)" >&2; exit 2; }

stage_common() {
    dir="$1"
    rm -rf "$dir"
    mkdir -p "$dir/Game"
    cp "$APK" "$dir/Game/pvz2.apk"
    [ -f "$REPO/runtime/tools/fonts/level-ui.ttf" ] && cp "$REPO/runtime/tools/fonts/level-ui.ttf" "$dir/"
    if [ -n "$TAR" ]; then
        [ -f "$TAR" ] || { echo "build-installer: no CDN archive at $TAR" >&2; exit 2; }
        cp "$TAR" "$dir/Game/$PROFILE_CDN_ARCHIVE"
    fi
    cat > "$dir/README.txt" <<'EOF'
@APP@ for Windows
=================

Run "@APP@.exe", or use the Start menu entry.

Where the game keeps things
  saves and settings   %APPDATA%\@DATA@
  game data, the OBB,
  the translation      %LOCALAPPDATA%\@DATA@
Uninstalling removes the program and the second folder; it asks about the
first one, which holds your saves, so a reinstall keeps them by default.

The first start on a 64-bit Intel/AMD PC translates the game, which takes a few
minutes; a small window shows how far it is. Later starts use the translation
that is kept in %LOCALAPPDATA%\@DATA@.

The Settings window (window menu, or Ctrl+,) changes the keys and the window.
The installed build checks for new releases once a day (Settings, About, turns
that off), and "Check for Updates..." in the window menu checks now.
"In a level the key that works each control is shown on it" is on by default
(Settings, Keyboard); level-ui.ttf beside the program draws those labels.
ANGLE, which draws the game through Direct3D 11, is licensed as LICENSE-ANGLE.txt says.
The first start downloads the game's data (the OBB, about 1.3 GB) once, with a
small progress window and a Cancel button; it is checked before the game starts.
The archive in Game is the offline fallback for the live content.
EOF
    sed "s|@APP@|$PROFILE_APP_NAME|g; s|@DATA@|$PROFILE_DATA_FOLDER|g" "$dir/README.txt" > "$dir/README.txt.new" && mv "$dir/README.txt.new" "$dir/README.txt"
}

# The x64 program is built with its own name in the zip; the installer has one folder per
# PC, so both programs are "PvZ2 Reflourished.exe" there.
# The licenses folder: our GPL, what both programs build in, and (x64) ScarletNX and the DLLs
# beside it (runtime/win/licenses/README.txt lists them and where their sources are).
stage_licenses() {
    lic="$1/licenses"
    mkdir -p "$lic"
    cp "$REPO/LICENSE" "$lic/$PROFILE_ARTIFACT.txt"
    cp "$REPO/runtime/win/licenses/"*.txt "$lic/"   # README.txt and the shared texts
    if [ "$2" = x64 ]; then cp "$REPO/runtime/win/licenses/x64/"*.txt "$lic/"; fi
}

stage_arm64() {
    dir="$1/arm64"
    stage_common "$dir"
    cp "$EXE_ARM" "$dir/$PROFILE_APP_NAME.exe"
    cp "$ANGLE/win-arm64"/*.dll "$dir/" 2>/dev/null || true
    [ -f "$ANGLE/win-arm64/LICENSE" ] && cp "$ANGLE/win-arm64/LICENSE" "$dir/LICENSE-ANGLE.txt" || true
    cp "$REPO/prebuilt/winsparkle/arm64/WinSparkle.dll" "$dir/"
    cp "$REPO/prebuilt/winsparkle/COPYING" "$dir/LICENSE-WinSparkle.txt"
    stage_licenses "$dir" arm64
}

stage_x64() {
    dir="$1/x64"
    stage_common "$dir"
    cp "$EXE_X64" "$dir/$PROFILE_APP_NAME.exe"
    cp "$ANGLE/win-x64"/*.dll "$dir/" 2>/dev/null || true
    [ -f "$ANGLE/win-x64/LICENSE" ] && cp "$ANGLE/win-x64/LICENSE" "$dir/LICENSE-ANGLE.txt" || true
    cp "$REPO/prebuilt/winsparkle/x64/WinSparkle.dll" "$dir/"
    cp "$REPO/prebuilt/winsparkle/COPYING" "$dir/LICENSE-WinSparkle.txt"
    [ -d "$X64_RUNTIME" ] && cp "$X64_RUNTIME"/*.dll "$dir/" 2>/dev/null || true
    stage_licenses "$dir" x64
}

mkdir -p "$OUT"
stage_arm64 "$OUT"
stage_x64 "$OUT"
printf '%s\n' "$VERSION" > "$OUT/version.txt"
# the names build-installer.cmd hands to Inno Setup (it has no Python or profile reader)
printf '%s\n' "AppName=$PROFILE_APP_NAME" "AppExe=$PROFILE_APP_NAME.exe" "AppUrl=https://github.com/$PROFILE_REPO" \
    "AppUserModelID=$PROFILE_AUMID" "Artifact=$PROFILE_ARTIFACT" "DataFolder=$PROFILE_DATA_FOLDER" > "$OUT/profile.txt"
python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$REPO/$PROFILE_GAME_DATA" > "$OUT/game-version.txt"
printf '%s\n' "$(cat "$REPO/prebuilt/winsparkle/.zip-sha256" 2>/dev/null)" > "$OUT/winsparkle-sha256.txt"

if [ "$MAKE_ZIP" = 1 ]; then
    rm -f "$OUT/stage.zip"
    (cd "$OUT" && zip -q -r -9 stage.zip arm64 x64 version.txt game-version.txt profile.txt winsparkle-sha256.txt)
    echo "staged $OUT and $OUT/stage.zip"
else
    echo "staged $OUT (arm64 and x64)"
fi
printf '%s\n' "on Windows: runtime\\win\\installer\\build-installer.cmd <the staged folder> $VERSION <output folder>"