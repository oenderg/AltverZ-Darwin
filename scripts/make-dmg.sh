#!/bin/bash
# Pack the Mac app into a disk image: the app and a link to Applications, so the
# player drags one onto the other. With dmgbuild (pip install dmgbuild) the window
# gets its lawn background and laid-out icons (scripts/dmg/); without it the image
# is a plain one with the same contents.
#
#   scripts/make-dmg.sh <app> <out.dmg>
set -euo pipefail
APP="$1" DMG="$2"
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
NAME="PvZ2 Reflourished"
HERE="$(cd "$(dirname "$0")" && pwd)"

rm -f "$DMG"
if python3 -c "import dmgbuild" 2>/dev/null; then
    python3 -m dmgbuild -s "$HERE/dmg/settings.py" -D app="$APP" \
        -D background="$HERE/dmg/background.tiff" "$NAME" "$DMG" >/dev/null
else
    echo "dmgbuild is not installed: a plain disk image, without the window's background" >&2
    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT
    ditto "$APP" "$STAGE/$(basename "$APP")"
    ln -s /Applications "$STAGE/Applications"
    # ULFO (lzfse) is read by every macOS this app runs on, and packs well.
    hdiutil create -quiet -volname "$NAME" -srcfolder "$STAGE" -fs APFS -format ULFO -ov "$DMG"
fi
hdiutil verify -quiet "$DMG"
echo "built: $DMG ($(du -h "$DMG" | cut -f1))"
