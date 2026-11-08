#!/bin/sh
# Put the pinned win32-darkmodelib commit in runtime/third_party/darkmodelib,
# with darkmodelib-delayload.patch applied.
#
# The sources are not in git (runtime/third_party/ is ignored), as with the
# iOS ANGLE prebuilts: this fetches the one commit the host's dark mode is
# tested with. It is a nested repository, so its own history stays out of this
# one. The patch is ours: a delay-loaded hook gets the target DLL's real
# export as its true function, not the loader's thunk (see the patch file).
set -e
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
DIR="$REPO/runtime/third_party/darkmodelib"
PATCH="$H/darkmodelib-delayload.patch"
PIN=e8ec95ba631462f01e238e63f7d06c1e6829a7a1
URL=https://github.com/ozone10/win32-darkmodelib.git

apply_patch() {
    if git -C "$DIR" apply --check "$PATCH" 2>/dev/null; then
        git -C "$DIR" apply "$PATCH"
    elif ! git -C "$DIR" apply --check --reverse "$PATCH" 2>/dev/null; then
        echo "darkmodelib: $PATCH does not apply to $PIN" >&2
        exit 1
    fi
}

if [ -d "$DIR/.git" ] && [ "$(git -C "$DIR" rev-parse HEAD 2>/dev/null)" = "$PIN" ]; then
    apply_patch
    echo "darkmodelib: $PIN"
    exit 0
fi
rm -rf "$DIR"
mkdir -p "$DIR"
git -C "$DIR" init -q
git -C "$DIR" remote add origin "$URL"
git -C "$DIR" fetch -q --depth 1 origin "$PIN"
git -C "$DIR" checkout -q FETCH_HEAD
got="$(git -C "$DIR" rev-parse HEAD)"
[ "$got" = "$PIN" ] || { echo "darkmodelib: HEAD is $got, expected $PIN" >&2; exit 1; }
apply_patch
echo "darkmodelib: $PIN"
