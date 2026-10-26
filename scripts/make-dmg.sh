#!/bin/bash
# Pack the Mac app into a disk image: the app and a link to Applications, so the
# player drags one onto the other. With dmgbuild (pip install dmgbuild) a profile
# whose dmg_art is true gets the lawn window and laid-out icons (scripts/dmg/); the
# other profiles, and a Mac without dmgbuild, get a plain image with the same
# contents. A profile's window art carries its product name, so it must not be used
# for another profile's app.
#
#   PROFILE=altverz scripts/make-dmg.sh <app> <out.dmg>
set -euo pipefail
APP="$1" DMG="$2"
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
NAME="$(basename "$APP" .app)"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# build.sh exports PROFILE; a standalone call means Reflourished, as before.
. "$ROOT/scripts/profile.sh"

rm -f "$DMG"
if python3 -c "import dmgbuild" 2>/dev/null && [ "$PROFILE_DMG_ART" = "True" ]; then
    python3 -m dmgbuild -s "$HERE/dmg/settings.py" -D app="$APP" \
        -D background="$HERE/dmg/background.tiff" "$NAME" "$DMG" >/dev/null
else
    if [ "$PROFILE_DMG_ART" != "True" ]; then
        echo "the $PROFILE profile has no disk-image art yet: a plain disk image" >&2
    else
        echo "dmgbuild is not installed: a plain disk image, without the window's background" >&2
    fi
    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT
    ditto "$APP" "$STAGE/$(basename "$APP")"
    ln -s /Applications "$STAGE/Applications"
    # ULFO (lzfse) is read by every macOS this app runs on, and packs well.
    hdiutil create -quiet -volname "$NAME" -srcfolder "$STAGE" -fs APFS -format ULFO -ov "$DMG"
fi
hdiutil verify -quiet "$DMG"
echo "built: $DMG ($(du -h "$DMG" | cut -f1))"
