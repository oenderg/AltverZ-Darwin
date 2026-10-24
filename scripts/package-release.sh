#!/bin/bash
# Move local IPA/DMG outputs to their public names; keep the app bundles for testing.
# (The AltStore source takes the app's icon from docs/banner/icon.png, not from a release.)
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
for src, dst in files:
    shutil.move(src, dst)
    print(dst)
PY
