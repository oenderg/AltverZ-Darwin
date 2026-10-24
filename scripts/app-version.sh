#!/bin/bash
# Shared bundle metadata, also used by the standalone app builders.
# VERSION owns the app version; the game manifest owns the game version.
VERSION_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_INFO="$(python3 - "$VERSION_ROOT" <<'PY'
import json
import os
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
version = (root / "VERSION").read_text().strip()
if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
    sys.exit("VERSION must contain major.minor.patch")
major, minor, patch = map(int, version.split("."))
if minor >= 100 or patch >= 100:
    sys.exit("VERSION minor and patch must be below 100 for the build number formula")
build = os.environ.get("APP_BUILD") or str(major * 10000 + minor * 100 + patch)
if not re.fullmatch(r"[1-9][0-9]*", build):
    sys.exit("APP_BUILD must be a positive integer")
game = json.loads((root / "data/game-1.4.2.json").read_text())["version"]
print(version, build, game)
PY
)" || return 1
read -r APP_VERSION APP_BUILD GAME_VERSION <<< "$VERSION_INFO"
export APP_VERSION APP_BUILD GAME_VERSION
