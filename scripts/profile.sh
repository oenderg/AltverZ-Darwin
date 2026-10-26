#!/bin/bash
# One mod, as the build and the runtime need to see it.
#
# Reflourished and AltVerZ are the same EA PvZ2 build behind a different mod library.
# Everything that differs -- package, versions, app identity, data folder, OBB, mod
# library and hook table, live-content host -- lives in data/profiles/<id>.json. The
# build scripts source this file, and the C side gets the same values through a
# generated pvz2-profile.h, so a change to a profile reaches the bundle, the driver
# and the hosts together.
#
#   PROFILE=altverz . scripts/profile.sh
#
# Without a profile the build is Reflourished's, exactly as before.
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
PROFILE="${PROFILE:-reflourished}"
PROFILE_FILE="$ROOT/data/profiles/$PROFILE.json"
[ -f "$PROFILE_FILE" ] || { echo "no mod profile at $PROFILE_FILE" >&2; return 1 2>/dev/null || exit 1; }

_profile() {
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); v=d.get(sys.argv[2]); print("" if v is None else v)' \
        "$PROFILE_FILE" "$1"
}
_profile_list() {
    python3 -c 'import json,sys; print("|".join(json.load(open(sys.argv[1])).get(sys.argv[2], [])))' \
        "$PROFILE_FILE" "$1"
}

PROFILE_ID="$PROFILE"
PROFILE_GAME_DATA="$(_profile game_data)"
PROFILE_HOOKS="$(_profile hooks)"
PROFILE_APP_NAME="$(_profile app_name)"
PROFILE_ARTIFACT="$(_profile artifact_name)"
PROFILE_BUNDLE_ID="$(_profile bundle_id)"
PROFILE_DATA_FOLDER="$(_profile data_folder)"
PROFILE_PACKAGE="$(_profile package_name)"
PROFILE_VERSION_CODE="$(_profile android_version_code)"
PROFILE_VERSION_NAME="$(_profile android_version_name)"
PROFILE_APP_VERSION="$(_profile app_version)"
PROFILE_MOD_LIBRARY="$(_profile mod_library)"
PROFILE_MOD_JNI_ONLOAD="$(_profile mod_jni_onload)"
PROFILE_ICON="$(_profile icon_path)"
PROFILE_CDN_HOSTS="$(_profile_list cdn_hosts)"
PROFILE_CDN_ARCHIVE="$(_profile cdn_archive)"
PROFILE_CDN_PATH="$(_profile cdn_path)"
PROFILE_SPARKLE="$(_profile sparkle)"
export PROFILE PROFILE_ID PROFILE_GAME_DATA PROFILE_HOOKS PROFILE_APP_NAME PROFILE_ARTIFACT
export PROFILE_BUNDLE_ID PROFILE_DATA_FOLDER PROFILE_PACKAGE PROFILE_VERSION_CODE
export PROFILE_VERSION_NAME PROFILE_APP_VERSION PROFILE_MOD_LIBRARY PROFILE_MOD_JNI_ONLOAD
export PROFILE_ICON PROFILE_CDN_HOSTS PROFILE_CDN_ARCHIVE PROFILE_CDN_PATH PROFILE_SPARKLE

# The C header and the Swift constants for this profile, written into a build folder.
profile_generate() {
    local dir="$1"
    python3 "$ROOT/scripts/profile-generate.py" "$PROFILE_FILE" "$dir" || return 1
    export PROFILE_DIR="$dir"
}
