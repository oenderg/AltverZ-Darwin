#!/bin/bash
# Draws the repository's banner (banner.html) with headless Google Chrome, at 2x: 2560x1280.
# Needs a Mac (the Apple logo on the icon is a character only Apple's system font draws)
# and, for the WebP files, cwebp (brew install webp).
#
#   ./render.sh                      the four banners: banner-{night,day}-{corner,center}.webp
#   ./render.sh out.png [options]    one banner as a PNG, e.g. "bg=day&at=center" (see banner.html)
#
# The inputs: mac.png (a window capture of the Mac app with its title bar, 2200x1076: a
# 1100x506-point window on a Retina display), phone.jpg (an iPhone 18 Pro simulator shot,
# `xcrun simctl io booted screenshot --mask=black`), settings.png (the Settings window),
# icon.png (the APK's res/mipmap-xxxhdpi/pvz2_icon.png), pvz2-word.png and logo-word.png
# (the title's lettering), bg-night.webp and bg-day.webp (lawn art).
set -euo pipefail
cd "$(dirname "$0")"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
[ -x "$CHROME" ] || { echo "needs Google Chrome in /Applications" >&2; exit 1; }

draw() {  # draw <out.png> <options>
    local out="$1"; [ "${out#/}" = "$out" ] && out="$PWD/$out"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --no-first-run \
        --user-data-dir="${TMPDIR:-/tmp}/banner-chrome" \
        --force-device-scale-factor=2 --window-size=1280,640 --virtual-time-budget=2000 \
        --screenshot="$out" "file://$PWD/banner.html?$2" >/dev/null 2>&1
    echo "$1: $(sips -g pixelWidth -g pixelHeight "$1" | awk '/pixel/ {printf "%s ", $2}')"
}

if [ $# -gt 0 ]; then
    draw "$1" "${2:-}"
    exit 0
fi
command -v cwebp >/dev/null || { echo "needs cwebp (brew install webp)" >&2; exit 1; }
for bg in night day; do
    for at in corner center; do
        png="$(mktemp -d)/banner.png"
        draw "$png" "bg=$bg&at=$at" >/dev/null
        cwebp -quiet -q 90 "$png" -o "banner-$bg-$at.webp"
        echo "banner-$bg-$at.webp: $(du -h "banner-$bg-$at.webp" | cut -f1)"
    done
done
