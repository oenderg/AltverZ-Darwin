#!/bin/sh
# Put WinSparkle's release files in prebuilt/winsparkle for the Windows program: download
# WinSparkle-<version>.zip from its GitHub release, check the sha256 that GitHub publishes for
# that asset, and unpack the ARM64 and x64 DLLs, their import libraries and the headers.
# Nothing happens when the folder already holds that version.
#
# WinSparkle is MIT licensed (COPYING); its own copy of expat is under COPYING.expat. The
# release also holds Win32 (x86) files and .pdb debug symbols, which the project does not use.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
VERSION=0.9.4
SUM=6037df37fc263bd1650a1c4949681a9d40ffe991d01f35892a406cb5d103c976
NAME="WinSparkle-$VERSION.zip"
CACHE="$REPO/out/win/winsparkle-download"
DIR="$REPO/prebuilt/winsparkle"

if [ -f "$DIR/arm64/WinSparkle.dll" ] && [ "$(cat "$DIR/.zip-sha256" 2>/dev/null)" = "$SUM" ]; then
    echo "fetch-winsparkle: $NAME is there"
    exit 0
fi
mkdir -p "$CACHE"
zip="$CACHE/$NAME"
if [ ! -f "$zip" ] || [ "$(shasum -a 256 "$zip" | cut -d' ' -f1)" != "$SUM" ]; then
    echo "fetch-winsparkle: downloading $NAME"
    curl -fL --retry 3 -o "$zip.part" \
        "https://github.com/vslavik/winsparkle/releases/download/v$VERSION/$NAME"
    mv "$zip.part" "$zip"
fi
got="$(shasum -a 256 "$zip" | cut -d' ' -f1)"
[ "$got" = "$SUM" ] || { echo "fetch-winsparkle: $NAME has sha256 $got, expected $SUM" >&2; rm -f "$zip"; exit 1; }

tmp="$CACHE/unpack"
rm -rf "$tmp"
mkdir -p "$tmp" "$DIR/arm64" "$DIR/x64" "$DIR/include"
unzip -oq "$zip" -d "$tmp"
src="$tmp/WinSparkle-$VERSION"
cp "$src/ARM64/Release/WinSparkle.dll" "$src/ARM64/Release/WinSparkle.lib" "$DIR/arm64/"
cp "$src/x64/Release/WinSparkle.dll" "$src/x64/Release/WinSparkle.lib" "$DIR/x64/"
cp "$src/include/winsparkle.h" "$src/include/winsparkle-version.h" "$DIR/include/"
cp "$src/COPYING" "$src/COPYING.expat" "$src/NEWS" "$DIR/"
printf '%s\n' "$SUM" > "$DIR/.zip-sha256"
rm -rf "$tmp"
echo "fetch-winsparkle: $NAME is in prebuilt/winsparkle"
