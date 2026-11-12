#!/bin/sh
# Put our ANGLE build in prebuilt/win-<arch> for the Windows zips: download the zip that
# runtime/win/ANGLE_ZIPS names from the draft release "build-inputs" (only people with write
# access see it; `gh` must be signed in), check its sha256, and unpack it. Nothing happens when
# the folder already holds that build. Usage: fetch-angle.sh [x64|arm64|all]
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
PROFILE="${PROFILE:-altverz}"; ROOT="$REPO"
. "$REPO/scripts/profile.sh"
REPOSITORY="${GITHUB_REPOSITORY:-$PROFILE_REPO}"
CACHE="$REPO/out/win/angle-download"
ARCH="${1:-all}"
[ "$ARCH" = all ] && set -- x64 arm64 || set -- "$ARCH"

for arch in "$@"; do
    line="$(grep -E "angle-windows-$arch-[0-9a-f]+\.zip\$" "$H/ANGLE_ZIPS" | head -1)"
    [ -n "$line" ] || { echo "fetch-angle: no $arch zip in runtime/win/ANGLE_ZIPS" >&2; exit 2; }
    sum="${line%% *}"; name="${line##* }"
    dir="$REPO/prebuilt/win-$arch"
    if [ -f "$dir/libGLESv2.dll" ] && [ "$(cat "$dir/.zip-sha256" 2>/dev/null)" = "$sum" ]; then
        echo "fetch-angle: $arch is $name"
        continue
    fi
    mkdir -p "$CACHE"
    zip="$CACHE/$name"
    if [ ! -f "$zip" ] || [ "$(shasum -a 256 "$zip" | cut -d' ' -f1)" != "$sum" ]; then
        command -v gh >/dev/null || { echo "fetch-angle: gh is needed to download $name" >&2; exit 2; }
        id="$(gh api "repos/$REPOSITORY/releases" --paginate \
            --jq ".[] | select(.draft and .tag_name == \"build-inputs\") | .assets[] | select(.name == \"$name\") | .id" | head -1)"
        [ -n "$id" ] || { echo "fetch-angle: no $name in the build-inputs draft release of $REPOSITORY" >&2; exit 2; }
        echo "fetch-angle: downloading $name"
        gh api -H "Accept: application/octet-stream" "repos/$REPOSITORY/releases/assets/$id" > "$zip.part"
        mv "$zip.part" "$zip"
    fi
    got="$(shasum -a 256 "$zip" | cut -d' ' -f1)"
    [ "$got" = "$sum" ] || { echo "fetch-angle: $name has sha256 $got, expected $sum" >&2; rm -f "$zip"; exit 1; }
    unzip -oq "$zip" -d "$dir"
    printf '%s\n' "$sum" > "$dir/.zip-sha256"
    echo "fetch-angle: $arch is $name"
done
