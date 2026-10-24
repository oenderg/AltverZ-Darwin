#!/bin/bash
# Sparkle, the Mac app's updater: the framework the app embeds and the tools that sign
# an update (bin/sign_update) and make the signing key (bin/generate_keys). The version
# is pinned and checked.
#
#   scripts/fetch-sparkle.sh <out-dir>      gives <out-dir>/Sparkle.framework and <out-dir>/bin
set -euo pipefail
OUT="${1:?usage: fetch-sparkle.sh <out-dir>}"
VERSION="2.10.0"
SHA256="c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c"
URL="https://github.com/sparkle-project/Sparkle/releases/download/$VERSION/Sparkle-$VERSION.tar.xz"

mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL --retry 3 -o "$TMP/sparkle.tar.xz" "$URL"
[ "$(shasum -a 256 "$TMP/sparkle.tar.xz" | cut -d' ' -f1)" = "$SHA256" ] || { echo "the Sparkle download does not match its checksum" >&2; exit 1; }
tar -xf "$TMP/sparkle.tar.xz" -C "$TMP" Sparkle.framework bin
rm -rf "$OUT/Sparkle.framework" "$OUT/bin"
# ditto keeps the framework's symlinks and its signature as they are
ditto "$TMP/Sparkle.framework" "$OUT/Sparkle.framework"
ditto "$TMP/bin" "$OUT/bin"
echo "$VERSION" > "$OUT/VERSION"
echo "    Sparkle $VERSION"
