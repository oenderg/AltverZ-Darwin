#!/bin/bash
# Pack the Mac app into a disk image: the app and a link to Applications, so the
# player drags one onto the other.
#
#   scripts/make-dmg.sh <app> <out.dmg>
set -euo pipefail
APP="$1" DMG="$2"
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
NAME="PvZ2 Reflourished"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/$(basename "$APP")"
ln -s /Applications "$STAGE/Applications"

rm -f "$DMG"
# ULFO (lzfse) is read by every macOS this app runs on, and packs well.
hdiutil create -quiet -volname "$NAME" -srcfolder "$STAGE" -fs APFS -format ULFO -ov "$DMG"
hdiutil verify -quiet "$DMG"
echo "built: $DMG ($(du -h "$DMG" | cut -f1))"
