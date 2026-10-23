#!/bin/bash
# ANGLE for the Mac: libEGL and libGLESv2 out of Chromium Embedded Framework's public
# build (the same major version Steam ships). Later CEF builds link ANGLE into the
# framework itself, so the version is pinned.
#
#   scripts/fetch-angle-macos.sh <out-dir>
set -euo pipefail
OUT="${1:?usage: fetch-angle-macos.sh <out-dir>}"
NAME="cef_binary_126.2.18+g3647d39+chromium-126.0.6478.183_macosarm64_minimal"
SHA1="740ac110ddd4cec6f691024d1b9f3b44a01e4cd9"
URL="https://cef-builds.spotifycdn.com/${NAME//+/%2B}.tar.bz2"

mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL --retry 3 -o "$TMP/cef.tar.bz2" "$URL"
[ "$(shasum "$TMP/cef.tar.bz2" | cut -d' ' -f1)" = "$SHA1" ] || { echo "the CEF download does not match its checksum" >&2; exit 1; }
LIBDIR="$NAME/Release/Chromium Embedded Framework.framework/Libraries"
tar -xjf "$TMP/cef.tar.bz2" -C "$TMP" "$LIBDIR/libEGL.dylib" "$LIBDIR/libGLESv2.dylib"
cp "$TMP/$LIBDIR/libEGL.dylib" "$TMP/$LIBDIR/libGLESv2.dylib" "$OUT/"
echo "    ANGLE: $(ls "$OUT" | tr '\n' ' ')"
