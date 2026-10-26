#!/bin/bash
# A PvZ2 mod as an .ipa, with the game inside it and the mod's icon.
#
#   tools/regress/build-ios-ipa.sh [out-dir]
#
# This is a thin layer over build-ios-app.sh: it works out where the game files and the
# icon are, and passes them on. The bundling, the icon and the signing all happen there,
# in that order, so the bundle is sealed exactly once.
#
#   GAME_APK   the game .apk (required)
#   GAME_OBB   the expansion file         (default: the main tar in /tmp; "none" for the slim
#              app, which downloads it on the first launch from OBB_URLS)
#   OBB_URLS, OBB_SHA256, OBB_SIZE   where the host downloads the expansion file from
#   OBB_NAME   its name (default: the profile package's)
#   SIGN_*     identity, provisioning profile, keychain: as for build-ios-app.sh
set -euo pipefail

R="$(cd "$(dirname "$0")/../.." && pwd)"
ROOT="$R/.."
. "$ROOT/scripts/profile.sh"
W="$(cd "$R/../../.." && pwd)"
OUT="${1:-$R/build/pvz2-ipa}"

GAME_APK="${GAME_APK:-}"
GAME_OBB="${GAME_OBB:-}"
export GAME_APK GAME_OBB

if [ -z "${ICON_PNG:-}" ]; then
    icon_dir="$OUT/icon"
    rm -rf "$icon_dir"; mkdir -p "$icon_dir"
    echo "==> taking the mod's icon out of the apk"
    unzip -o -q "$GAME_APK" "$PROFILE_ICON" -d "$icon_dir"
    ICON_PNG="$icon_dir/$PROFILE_ICON"
fi
export ICON_PNG

"$R/tools/regress/build-ios-app.sh" "$OUT"

if [ "${IOS_SIM:-0}" = 1 ]; then
    echo
    echo "install the simulator bundle directly:"
    echo "  xcrun simctl install booted \"${IOS_SIM_APP:-$OUT/$PROFILE_ARTIFACT-sim.app}\""
    exit 0
fi

echo
echo "install the bundle directly:"
echo "  xcrun devicectl device install app --device <udid> $OUT/PvZ2.app"
echo "or hand the ipa to a sideloader, which re-signs nested code and brings its own profile:"
echo "  $OUT/PvZ2.ipa"
