#!/bin/sh
# Unpack the MSYS2 CLANG64 packages the cross-built x86-64 Windows program links against and ships
# with, on a Mac or Linux (no Windows, no MSYS2):
#
#   runtime/win/x64/cross/setup-sysroot.sh <folder>
#
# Every package named in msys2-sysroot.txt is downloaded from repo.msys2.org (MSYS2_REPO overrides the
# address), checked against the SHA-256 pinned there, and unpacked into <folder> (it makes <folder>/clang64).
# A folder that already holds exactly this set is left alone. Needs curl, zstd, tar and shasum or sha256sum.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:?usage: setup-sysroot.sh <folder>}"
LIST="$H/msys2-sysroot.txt"
BASE="${MSYS2_REPO:-https://repo.msys2.org/mingw/clang64}"
P=mingw-w64-clang-x86_64
STAMP="$DIR/.sysroot-packages"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

want="$(grep -v '^#' "$LIST" | grep -v '^$' | cut -d' ' -f1,2)"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ] && [ -d "$DIR/clang64/lib/cmake/llvm" ]; then
    echo "setup-sysroot: $DIR is current"
    exit 0
fi
for tool in curl zstd tar; do
    command -v "$tool" >/dev/null || { echo "setup-sysroot: $tool is needed" >&2; exit 1; }
done

if [ -e "$DIR" ] && [ ! -f "$STAMP" ] && [ -n "$(ls -A "$DIR" 2>/dev/null)" ]; then
    echo "setup-sysroot: $DIR exists and is not a sysroot this script made; use an empty or new folder" >&2
    exit 1
fi
rm -rf "$DIR.partial"
mkdir -p "$DIR.partial/pkgs" "$DIR.partial/root"
grep -v '^#' "$LIST" | grep -v '^$' | while read -r name version hash; do
    file="$P-$name-$version-any.pkg.tar.zst"
    echo "  $name $version"
    curl -fsSL --retry 3 -o "$DIR.partial/pkgs/$file" "$BASE/$file"
    got="$(sha256 "$DIR.partial/pkgs/$file")"
    if [ "$got" != "$hash" ]; then
        echo "setup-sysroot: $file has sha256 $got, expected $hash" >&2
        exit 1
    fi
    zstd -dc "$DIR.partial/pkgs/$file" | tar -x -C "$DIR.partial/root"
done
[ -f "$DIR.partial/root/clang64/bin/libLLVM-23.dll" ] || { echo "setup-sysroot: libLLVM-23.dll is missing from the unpacked packages" >&2; exit 1; }
rm -rf "$DIR"
mv "$DIR.partial/root" "$DIR"
rm -rf "$DIR.partial"
printf '%s\n' "$want" > "$STAMP"
echo "setup-sysroot: $(printf '%s\n' "$want" | wc -l | tr -d ' ') packages in $DIR/clang64"
