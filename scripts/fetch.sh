#!/bin/bash
# Download one file from a link: a direct link, a Google Drive share link, or a MEGA link.
#
#   scripts/fetch.sh <url> <dest>
set -euo pipefail
URL="${1:?usage: fetch.sh <url> <dest>}"
DEST="${2:?usage: fetch.sh <url> <dest>}"
mkdir -p "$(dirname "$DEST")"
echo "    downloading $(basename "$DEST") from ${URL%%\?*}"

case "$URL" in
    *mega.nz*|*mega.co.nz*)
        command -v megatools >/dev/null || command -v megadl >/dev/null || {
            echo "a MEGA link needs megatools (brew install megatools)" >&2; exit 1; }
        TMP="$(mktemp -d)"
        trap 'rm -rf "$TMP"' EXIT
        if command -v megatools >/dev/null; then megatools dl --no-progress --path "$TMP" "$URL"
        else megadl --no-progress --path "$TMP" "$URL"; fi
        f="$(find "$TMP" -type f | head -1)"
        [ -n "$f" ] || { echo "nothing was downloaded from $URL" >&2; exit 1; }
        mv "$f" "$DEST"
        ;;
    *drive.google.com*|*docs.google.com*)
        # the id is in /d/<id>/ or ?id=<id>; large files want confirm=t
        id="$(printf '%s' "$URL" | sed -nE 's#.*/d/([^/?]+).*#\1#p')"
        [ -n "$id" ] || id="$(printf '%s' "$URL" | sed -nE 's#.*[?&]id=([^&]+).*#\1#p')"
        [ -n "$id" ] || { echo "cannot find the file id in $URL" >&2; exit 1; }
        curl -fL --retry 3 -o "$DEST" "https://drive.usercontent.google.com/download?id=$id&export=download&confirm=t"
        if head -c 512 "$DEST" | grep -qi '<html'; then
            echo "Google Drive answered with a web page instead of the file (is the link shared publicly?)" >&2
            rm -f "$DEST"; exit 1
        fi
        ;;
    *)
        curl -fL --retry 3 -o "$DEST" "$URL"
        ;;
esac
echo "    $(basename "$DEST"): $(du -h "$DEST" | cut -f1)"
