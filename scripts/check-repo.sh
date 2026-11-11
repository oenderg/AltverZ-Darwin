#!/bin/bash
# Quick consistency checks that need no build, run on every push and as the release's first step:
#
#   scripts/check-repo.sh
#
# - the release notes for VERSION exist;
# - the game version (data/game-<version>.json) is the one build.sh, the runtime, the version
#   script and the README use, including the README badge's link to the mod's changelog;
# - every DLL the x64 Windows build ships has its license in runtime/win/licenses;
# - sparkle/public-key is a 32-byte Ed25519 key, and both Windows builds and the Mac build read it;
# - the release text builds for a release with all seven files.
#
# Prints each problem and exits 1 if there is any.
set -uo pipefail
cd "$(dirname "$0")/.."
bad=0
problem() { echo "check-repo: $*" >&2; bad=1; }

# The release notes
VERSION="$(cat VERSION)"
[ -f "release-notes/$VERSION.md" ] || problem "VERSION is $VERSION but release-notes/$VERSION.md is missing"

# The game version
games=(data/game-*.json)
if [ "${#games[@]}" -ne 1 ]; then
    problem "expected one data/game-*.json, found ${#games[@]}: ${games[*]}"
else
    G="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["version"])' "${games[0]}")"
    [ "${games[0]}" = "data/game-$G.json" ] || problem "${games[0]} says version $G"
    [ -f "data/hooks-$G.json" ] || problem "data/hooks-$G.json is missing (the game is $G)"
    for f in build.sh scripts/app-version.sh runtime/src/translation-layer-next/husk-tl-pvz2.c; do
        grep -q "$G" "$f" || problem "$f does not mention the game version $G"
    done
    grep -q "/badge/PvZ2%20Reflourished-$G-" README.md || problem "the README badge does not say $G"
    SERIES="${G%.*}.x"
    grep -q "reflourished.fandom.com/wiki/Changelogs/Version_$SERIES" README.md ||
        problem "the README badge does not link to the changelog of $SERIES"
    grep -q "PvZ2 Reflourished $G" README.md || problem "the README's build steps do not name the $G APK"
fi

# The x64 DLLs' licenses (collect-dlls.sh stops the build when the set changes; this says so sooner)
license_of() {
    case "$1" in
        libc++.dll) echo x64/libc++.txt ;;   libffi-8.dll) echo x64/libffi.txt ;;
        libiconv-2.dll) echo x64/libiconv.txt ;;   libllvm-23.dll) echo x64/LLVM.txt ;;
        libwinpthread-1.dll) echo x64/winpthreads.txt ;;   libxml2-16.dll) echo x64/libxml2.txt ;;
        libzstd.dll) echo zstd.txt ;;   zlib1.dll) echo x64/zlib.txt ;;
    esac
}
expected="$(sed -n 's/^EXPECTED="\(.*\)"$/\1/p' runtime/win/x64/collect-dlls.sh)"
[ -n "$expected" ] || problem "no EXPECTED list in runtime/win/x64/collect-dlls.sh"
for dll in $expected; do
    lic="$(license_of "$dll")"
    if [ -z "$lic" ]; then
        problem "$dll ships with the x64 build but has no license here: add it to runtime/win/licenses and to this list"
    elif [ ! -f "runtime/win/licenses/$lic" ]; then
        problem "runtime/win/licenses/$lic ($dll's license) is missing"
    fi
done

# The update key
python3 - sparkle/public-key <<'PY' || bad=1
import base64, sys
try:
    raw = base64.b64decode(open(sys.argv[1]).read().strip(), validate=True)
except Exception as e:
    sys.exit(f"check-repo: {sys.argv[1]} is not base64: {e}")
if len(raw) != 32:
    sys.exit(f"check-repo: {sys.argv[1]} is {len(raw)} bytes, not a 32-byte Ed25519 key")
PY
for f in runtime/win/build-win.sh runtime/win/x64/build-x64.sh runtime/tools/regress/build-macos-app.sh; do
    grep -q "sparkle/public-key" "$f" || problem "$f does not take its update key from sparkle/public-key"
done

# The release text, for a release with every kind of file
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
P="PvZ2-Reflourished-${G:-0}-Darwin-$VERSION"
python3 - "$tmp/assets.json" "$P" <<'PY'
import json, sys
ends = ["iOS.ipa", "iOS-Full.ipa", "Mac.dmg", "Mac-Full.dmg", "Windows-Setup.exe", "Windows-x64.zip", "Windows-ARM64.zip"]
json.dump({"assets": [{"name": f"{sys.argv[2]}-{e}", "size": 1000000} for e in ends]}, open(sys.argv[1], "w"))
PY
if python3 scripts/release-notes.py --assets "$tmp/assets.json" --version "$VERSION" --tag "v$VERSION" \
    --since HEAD > "$tmp/notes.md" 2> "$tmp/err"; then
    [ "$(grep -c "^| .* | \`$P-" "$tmp/notes.md")" = 7 ] || problem "the release text does not list all seven files"
    head -1 "$tmp/notes.md" | grep -q '^\*\*Download:\*\* .*Windows' || problem "the release text does not start with the download links"
    if grep -n '{downloads}\|{repo}' "$tmp/notes.md" >&2; then problem "the release text still has a placeholder"; fi
else
    problem "scripts/release-notes.py failed: $(cat "$tmp/err")"
fi

files="$(git ls-files '*.md' 'release-notes/*' | grep -v -e '^runtime/tools/vendor/' -e '^converter/third_party/')"
hits="$(printf '%s\n' "$files" | xargs grep -n $'\xe2\x80\x94' 2>/dev/null)"
[ -z "$hits" ] || { printf '%s\n' "$hits" | sed 's/^/check-repo: U+2014 at /' >&2; bad=1; }

[ "$bad" = 0 ] && echo "check-repo: all good"
exit "$bad"
