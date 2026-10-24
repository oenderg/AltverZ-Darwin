#!/bin/bash
# PvZ2 Reflourished as an .ipa, with the game inside it and Reflourished's icon.
#
#   tools/regress/build-ios-ipa.sh [out-dir]
#
# This is a thin layer over build-ios-app.sh: it works out where the game files and the
# icon are, and passes them on. The bundling, the icon and the signing all happen there,
# in that order, so the bundle is sealed exactly once.
#
#   GAME_APK   the Reflourished .apk      (default: the one in the workspace)
#   GAME_OBB   the expansion file         (default: the main tar in /tmp; "none" for the slim
#              app, which downloads it on the first launch from OBB_URLS)
#   OBB_URLS, OBB_SHA256, OBB_SIZE   where the host downloads the expansion file from
#   SIGN_*     identity, provisioning profile, keychain: as for build-ios-app.sh
set -euo pipefail

R="$(cd "$(dirname "$0")/../.." && pwd)"
W="$(cd "$R/../../.." && pwd)"
OUT="${1:-$R/build/pvz2-ipa}"

GAME_APK="${GAME_APK:-$W/user/PvZ2 Reflourished 1.4.2.apk}"
GAME_OBB="${GAME_OBB:-/tmp/1.4.2.main.tar}"
export GAME_APK GAME_OBB

if [ -z "${ICON_PNG:-}" ]; then
    icon_dir="$OUT/icon"
    rm -rf "$icon_dir"; mkdir -p "$icon_dir"
    echo "==> taking Reflourished's icon out of the apk"
    unzip -o -q "$GAME_APK" "res/mipmap-xxxhdpi/pvz2_icon.png" -d "$icon_dir"
    ICON_PNG="$icon_dir/res/mipmap-xxxhdpi/pvz2_icon.png"
fi
export ICON_PNG

"$R/tools/regress/build-ios-app.sh" "$OUT"

if [ "${IOS_SIM:-0}" = 1 ]; then
    echo
    echo "install the simulator bundle directly:"
    echo "  xcrun simctl install booted \"${IOS_SIM_APP:-$OUT/PvZ2-Reflourished-sim.app}\""
    exit 0
fi

echo
echo "install the bundle directly:"
echo "  xcrun devicectl device install app --device <udid> $OUT/PvZ2.app"
echo "or hand the ipa to a sideloader, which re-signs nested code and brings its own profile:"
echo "  $OUT/PvZ2.ipa"
