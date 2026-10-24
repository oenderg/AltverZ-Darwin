#!/bin/bash
# Move local IPA/DMG outputs to their public names; keep the app bundles for testing.
#   scripts/package-release.sh <out-dir> <release-dir> slim|full
set -euo pipefail
[ "$#" = 3 ] || { echo "usage: $0 <out-dir> <release-dir> slim|full" >&2; exit 2; }
case "$3" in slim|full) ;; *) echo "variant must be slim or full" >&2; exit 2 ;; esac
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/app-version.sh"
python3 - "$1" "$2" "$3" "$APP_VERSION" "$GAME_VERSION" <<'PY'
from pathlib import Path
import shutil
import sys
import zipfile

out, release = map(Path, sys.argv[1:3])
variant, version, game = sys.argv[3:]
prefix = f"PvZ2-Reflourished-{game}-Darwin-{version}"
suffix = "-Full" if variant == "full" else ""
files = [(out / f"PvZ2-Reflourished.{ext}", release / f"{prefix}-{platform}{suffix}.{ext}")
         for ext, platform in [("ipa", "iOS"), ("dmg", "Mac")]
         if (out / f"PvZ2-Reflourished.{ext}").is_file()]
if not files:
    sys.exit("no IPA or DMG to package")
for src, dst in files:
    if dst.exists():
        sys.exit(f"refusing to replace {dst}")
release.mkdir(parents=True, exist_ok=True)
icon = release / "PvZ2-Reflourished-icon.png"
if not icon.exists():
    local_icon = out / icon.name
    if local_icon.is_file():
        shutil.copyfile(local_icon, icon)
    else:
        apks = [out / ".work/game.apk", out / "PvZ2 Reflourished.app/Contents/Game/pvz2.apk",
                out / ".work/ios/PvZ2.app/Game/pvz2.apk"]
        apk = next((p for p in apks if p.is_file()), None)
        if apk is None:
            sys.exit("no APK or local icon found for the release icon")
        with zipfile.ZipFile(apk) as z:
            icon.write_bytes(z.read("res/mipmap-xxxhdpi/pvz2_icon.png"))
for src, dst in files:
    shutil.move(src, dst)
    print(dst)
print(icon)
PY
