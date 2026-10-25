#!/bin/bash
# Draws the disk image's window background (dmg.html) with headless Google Chrome and writes
# background.tiff: the 720x440 picture at 1x and 2x, which Finder picks by the display.
# Needs a Mac (tiffutil), Google Chrome, and the game's art and UI font in art/ (decoded from the
# game's OBB, so not in the repository; art/ is ignored). The release build only reads background.tiff.
#
#   ./render.sh                 background.tiff
#   ./render.sh preview.png [app]   one 2x picture with the two icons drawn in, to judge the
#                                   layout (the app's own icon; default: out/PvZ2 Reflourished.app)
set -euo pipefail
cd "$(dirname "$0")"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
[ -x "$CHROME" ] || { echo "needs Google Chrome in /Applications" >&2; exit 1; }
for f in scene.jpg grass.webp peashooter.png sunflower.png zombie-basic.png zombie-cone.png zombie-bucket.png pvz2-ui.ttf; do
    [ -f "art/$f" ] || { echo "needs the game's art in art/ (missing $f; see dmg.html)" >&2; exit 1; }
done

draw() {  # draw <out.png> [query]
    local out="$1"; [ "${out#/}" = "$out" ] && out="$PWD/$out"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --no-first-run \
        --user-data-dir="${TMPDIR:-/tmp}/dmg-chrome" \
        --force-device-scale-factor=2 --window-size=720,440 --virtual-time-budget=3000 \
        --screenshot="$out" "file://$PWD/dmg.html${2:-}" >/dev/null 2>&1
}

if [ $# -gt 0 ]; then
    # The icons as Finder draws them: the app's and the Applications folder's (not kept in
    # the repository).
    APP="${2:-../../out/PvZ2 Reflourished.app}"
    [ -d "$APP" ] || { echo "no app at $APP (build one, or name one)" >&2; exit 1; }
    swift - "$APP" "$PWD/preview-app.png" /Applications "$PWD/preview-folder.png" <<'EOF'
import AppKit
let a = CommandLine.arguments
for i in stride(from: 1, to: a.count, by: 2) {
    let icon = NSWorkspace.shared.icon(forFile: a[i])
    icon.size = NSSize(width: 256, height: 256)
    let png = NSBitmapImageRep(data: icon.tiffRepresentation!)!.representation(using: .png, properties: [:])!
    try! png.write(to: URL(fileURLWithPath: a[i + 1]))
}
EOF
    draw "$1" "?preview=1"
    rm -f preview-app.png preview-folder.png
    echo "$1"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
draw "$TMP/background@2x.png"
sips -z 440 720 "$TMP/background@2x.png" --out "$TMP/background.png" >/dev/null
tiffutil -cathidpicheck "$TMP/background.png" "$TMP/background@2x.png" -out background.tiff 2>/dev/null
echo "background.tiff: $(du -h background.tiff | cut -f1)"
