#!/bin/bash
# Install the CLANG64 packages the x86-64 build is made with, at exactly the versions
# runtime/win/x64/msys2-clang64.txt names, from MSYS2's own repository. Run it in an MSYS2
# shell (the release workflow does, on a fresh MSYS2):
#
#   runtime/win/x64/setup-msys2.sh
#
# MSYS2 keeps only rolling versions in its package database, so `pacman -S` would install
# whatever is newest that day. Each package is named by its file instead (pacman -U), and
# pacman checks every file's signature against MSYS2's keyring. The script stops when any
# listed package ends up at another version.
set -euo pipefail
H="$(cd "$(dirname "$0")" && pwd)"
LIST="$H/msys2-clang64.txt"
BASE="${MSYS2_REPO:-https://repo.msys2.org/mingw/clang64}"
P=mingw-w64-clang-x86_64

urls=()
while read -r name version; do
    case "$name" in ''|'#'*) continue ;; esac
    urls+=("$BASE/$P-$name-$version-any.pkg.tar.zst")
done < "$LIST"
pacman -U --noconfirm --needed "${urls[@]}"

bad=0
while read -r name version; do
    case "$name" in ''|'#'*) continue ;; esac
    got="$(pacman -Q "$P-$name" 2>/dev/null | cut -d' ' -f2)"
    if [ "$got" != "$version" ]; then
        echo "setup-msys2: $P-$name is ${got:-missing}, expected $version" >&2
        bad=1
    fi
done < "$LIST"
[ "$bad" = 0 ] || exit 1
echo "setup-msys2: ${#urls[@]} CLANG64 packages at the pinned versions"
