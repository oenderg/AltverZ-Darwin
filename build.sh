#!/bin/bash
# Build PvZ2: Reflourished for iPhone, iPad and Mac from the Android release.
#
#   ./build.sh --apk <file|url> [--obb <file|url>] [--cdn <file|url>] [--ios] [--ios-sim] [--mac] [--slim] [--quick] [--reuse-converted] [--out dir]
#
# Without --ios, --ios-sim or --mac, iOS and Mac are built. --apk and --obb take a file, or a link
# (direct, Google Drive or MEGA) that is downloaded first; without --obb the OBB
# comes from the mod's own release. --slim makes the apps without the OBB inside
# (the IPA is about 60 MB, the Mac disk image about 65 MB): they download it themselves
# on their first launch. --quick reuses the game's converted code from the last
# build in the same --out and skips the disk image: the fast loop while working
# on runtime/. VERSION sets our version; data/game-1.4.2.json sets the game version.
# --reuse-converted reuses the converted code but still makes the disk image (release variants).
# scripts/package-release.sh gives the IPA and DMG their versioned release names.
# APP_BUILD overrides the build number (default: major*10000 + minor*100 + patch).
# The results land in out/ (or --out):
#
#   PvZ2-Reflourished.ipa          for iPhone and iPad (sign it with your sideloader)
#   PvZ2-Reflourished-sim.app      for the iOS Simulator (Apple silicon, --ios-sim)
#   PvZ2 Reflourished.app          for the Mac (Apple silicon)
#   PvZ2-Reflourished.dmg          the same app in a disk image, ready to share
#
# Needs: a Mac with Xcode (and its iOS platform for --ios), python3, about 8 GB free.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
. "$ROOT/scripts/app-version.sh"
OUT="$ROOT/out"
APK="" OBB="" CDN="" WANT_IOS=0 WANT_IOS_SIM=0 WANT_MAC=0 SLIM=0 QUICK=0 REUSE_CONVERTED=0

while [ $# -gt 0 ]; do
    case "$1" in
        --apk) APK="$2"; shift 2 ;;
        --obb) OBB="$2"; shift 2 ;;
        --cdn) CDN="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --ios) WANT_IOS=1; shift ;;
        --ios-sim) WANT_IOS_SIM=1; shift ;;
        --mac) WANT_MAC=1; shift ;;
        --slim) SLIM=1; shift ;;
        --quick) QUICK=1; shift ;;
        --reuse-converted) REUSE_CONVERTED=1; shift ;;
        -h|--help) sed -n '2,/^set -euo pipefail/{ /^set -euo pipefail/d; p; }' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see ./build.sh --help)" >&2; exit 2 ;;
    esac
done
[ "$WANT_IOS$WANT_IOS_SIM$WANT_MAC" = 000 ] && WANT_IOS=1 WANT_MAC=1
[ -n "$APK" ] || { echo "--apk is required (see ./build.sh --help)" >&2; exit 2; }

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$OUT/.work"
mkdir -p "$WORK"
GAME="$ROOT/data/game-1.4.2.json"
HOOKS="$ROOT/data/hooks-1.4.2.json"
json() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))" "$GAME" "$1"; }
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- the game files
fetch() {   # fetch <file|url> <dest>: a local file is used as it is
    local src="$1" dest="$2"
    if [ -f "$src" ]; then echo "$src"; return; fi
    case "$src" in http*://*|*mega.nz*) ;; *) echo "no such file: $src" >&2; exit 1 ;; esac
    if [ ! -s "$dest" ]; then
        bash "$ROOT/scripts/fetch.sh" "$src" "$dest" >&2
    fi
    echo "$dest"
}
step "the game files"
APK="$(fetch "$APK" "$WORK/game.apk")"
# The OBB: needed for full apps, not for slim ones (they download it themselves).
NEED_OBB=1
[ "$SLIM" = 1 ] && NEED_OBB=0
if [ "$NEED_OBB" = 1 ]; then
    [ -n "$OBB" ] || OBB="$(json 'd["obb_urls"][0]')"
    OBB="$(fetch "$OBB" "$WORK/game.obb")"
fi
if [ -z "$CDN" ]; then
    CDN="$WORK/1.4.2.main.tar"
    [ -s "$CDN" ] || curl -fL --retry 3 -o "$CDN" "$(json 'd["cdn_url"]')"
else
    CDN="$(fetch "$CDN" "$WORK/1.4.2.main.tar")"
fi

# The mod's hooks were recorded against one exact build of the game's code, so the
# APK has to be that build: anything else would start and then fail in strange ways.
rm -rf "$WORK/libs"; mkdir -p "$WORK/libs"
unzip -o -q "$APK" 'lib/arm64-v8a/*.so' -d "$WORK/libs" || { echo "$APK is not an Android APK with arm64 libraries" >&2; exit 1; }
LIBS="$WORK/libs/lib/arm64-v8a"
for so in libPVZ2.so libRFLib.so libNimble.so libc++_shared.so; do
    want="$(json "d['libs_sha256']['$so']")"
    have="$(shasum -a 256 "$LIBS/$so" 2>/dev/null | cut -d' ' -f1)"
    if [ "$want" != "$have" ]; then
        echo "this APK's $so is not the one Reflourished $(json 'd["version"]') ships." >&2
        echo "Use the PvZ2 Reflourished $(json 'd["version"]') APK from the mod's official downloads." >&2
        exit 1
    fi
done
if [ "$NEED_OBB" = 1 ]; then
    obb_size="$(stat -f %z "$OBB")"
    [ "$obb_size" -gt 100000000 ] || { echo "$OBB is $obb_size bytes; the game's OBB is about 1.3 GB" >&2; exit 1; }
    if [ "$(shasum -a 256 "$OBB" | cut -d' ' -f1)" != "$(json 'd["obb_sha256"]')" ]; then
        echo "    note: this OBB is not Reflourished $(json 'd["version"]')'s own (it may be another version's)." >&2
    fi
    echo "    APK: Reflourished $(json 'd["version"]') ✓    OBB: $(du -h "$OBB" | cut -f1)    CDN archive: $(du -h "$CDN" | cut -f1)"
else
    echo "    APK: Reflourished $(json 'd["version"]') ✓    OBB: downloaded by the app    CDN archive: $(du -h "$CDN" | cut -f1)"
fi
# Where the apps download the OBB from when it is not inside them (or not the
# right one): the links in order, and the size and sha256 it must have.
OBB_URLS="$(python3 -c "import json,sys; print(' '.join(json.load(open(sys.argv[1]))['obb_urls']))" "$GAME")"
OBB_SHA256="$(json 'd["obb_sha256"]')"
OBB_SIZE="$(json 'd["obb_size"]')"

# ---------------------------------------------------------------- iPhone and iPad
if [ "$WANT_IOS" = 1 ] || [ "$WANT_IOS_SIM" = 1 ]; then
    if [ "$WANT_IOS_SIM" = 1 ]; then
        xcrun --sdk iphonesimulator --show-sdk-path >/dev/null 2>&1 || {
            echo "Xcode's iOS Simulator SDK is missing" >&2; exit 1; }
    fi
    xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1 || {
        echo "Xcode's iOS platform is missing (Xcode > Settings > Components)" >&2; exit 1; }

    ANGLE_IOS="$ROOT/runtime/build/ios-arm64/sysroot/lib/libANGLE-shared.dylib"
    if [ ! -f "$ANGLE_IOS" ] && [ -f "$ROOT/prebuilt/ios/libANGLE-shared.dylib" ] && [ -z "${REBUILD_ANGLE:-}" ]; then
        mkdir -p "$(dirname "$ANGLE_IOS")"
        cp "$ROOT/prebuilt/ios/libANGLE-shared.dylib" "$ANGLE_IOS"
    fi
    if [ ! -f "$ANGLE_IOS" ]; then
        step "ANGLE for iOS (once; it takes a while)"
        bash "$ROOT/runtime/scripts/build_angle_ios.sh"
    fi
    if [ "$WANT_IOS_SIM" = 1 ]; then
        # The simulator needs its own ANGLE: a device one aborts on the first 16-bit texture.
        ANGLE_SIM="$ROOT/runtime/build/iossim-arm64/sysroot/lib/libANGLE-shared.dylib"
        if [ ! -f "$ANGLE_SIM" ] && [ -f "$ROOT/prebuilt/ios-simulator/libANGLE-shared.dylib" ] && [ -z "${REBUILD_ANGLE:-}" ]; then
            mkdir -p "$(dirname "$ANGLE_SIM")"
            cp "$ROOT/prebuilt/ios-simulator/libANGLE-shared.dylib" "$ANGLE_SIM"
        fi
        if [ ! -f "$ANGLE_SIM" ]; then
            step "ANGLE for the iOS Simulator (once; it takes a while)"
            ANGLE_SDK=iphonesimulator bash "$ROOT/runtime/scripts/build_angle_ios.sh"
        fi
    fi

    if { [ "$QUICK" = 1 ] || [ "$REUSE_CONVERTED" = 1 ]; } && [ -f "$WORK/ios-dylibs/libPVZ2.dylib" ]; then
        step "the game's code for iOS: reused from the last build"
    else
        step "converting the game's code for iOS"
        HOOKS="$HOOKS" bash "$ROOT/converter/build-ios-dylibs.sh" "$LIBS" "$WORK/ios-dylibs"
        python3 "$ROOT/converter/check-build.py" "$WORK/ios-dylibs"
    fi

    IOS_OBB="$OBB"; [ "$SLIM" = 1 ] && IOS_OBB=none
    if [ "$WANT_IOS" = 1 ]; then
        step "building the iOS app"
        GAME_APK="$APK" GAME_OBB="$IOS_OBB" GAME_CDN_TAR="$CDN" \
        OBB_URLS="$OBB_URLS" OBB_SHA256="$OBB_SHA256" OBB_SIZE="$OBB_SIZE" \
        AOT_DYLIB_DIR="$WORK/ios-dylibs" BUNDLE_ID="${BUNDLE_ID:-io.github.oenderg.reflourished-darwin}" \
            bash "$ROOT/runtime/tools/regress/build-ios-ipa.sh" "$WORK/ios" | grep -E '^==>|^built|^ipa'
        [ -f "$WORK/ios/PvZ2.ipa" ] || { echo "the iOS build failed (see above)" >&2; exit 1; }
        mv -f "$WORK/ios/PvZ2.ipa" "$OUT/PvZ2-Reflourished.ipa"
    fi

    if [ "$WANT_IOS_SIM" = 1 ]; then
        step "building the iOS Simulator app"
        GAME_APK="$APK" GAME_OBB="$IOS_OBB" GAME_CDN_TAR="$CDN" \
        OBB_URLS="$OBB_URLS" OBB_SHA256="$OBB_SHA256" OBB_SIZE="$OBB_SIZE" \
        AOT_DYLIB_DIR="$WORK/ios-dylibs" BUNDLE_ID="${BUNDLE_ID:-io.github.oenderg.reflourished-darwin}" \
        IOS_SIM=1 IOS_SIM_APP="$OUT/PvZ2-Reflourished-sim.app" \
            bash "$ROOT/runtime/tools/regress/build-ios-ipa.sh" "$WORK/ios-sim"
    fi
fi

# ---------------------------------------------------------------- Mac
if [ "$WANT_MAC" = 1 ]; then
    ANGLE_MAC="$WORK/angle-macos"
    if [ ! -f "$ANGLE_MAC/libGLESv2.dylib" ]; then
        step "ANGLE for the Mac (from Chromium's public build)"
        bash "$ROOT/scripts/fetch-angle-macos.sh" "$ANGLE_MAC"
    fi

    if { [ "$QUICK" = 1 ] || [ "$REUSE_CONVERTED" = 1 ]; } && [ -f "$WORK/macos-dylibs/libPVZ2.dylib" ]; then
        step "the game's code for the Mac: reused from the last build"
    else
        step "converting the game's code for the Mac"
        HOOKS="$HOOKS" GLES="$ANGLE_MAC/libGLESv2.dylib" \
            bash "$ROOT/converter/build-macos-dylibs.sh" "$LIBS" "$WORK/macos-dylibs"
    fi

    step "building the Mac app"
    MAC_OBB="$OBB"; [ "$SLIM" = 1 ] && MAC_OBB=""
    GAME_APK="$APK" GAME_OBB="$MAC_OBB" GAME_CDN_TAR="$CDN" AOT_DYLIB_DIR="$WORK/macos-dylibs" \
    OBB_URLS="$OBB_URLS" OBB_SHA256="$OBB_SHA256" OBB_SIZE="$OBB_SIZE" \
    ANGLE_EGL="$ANGLE_MAC/libEGL.dylib" ANGLE_GLES="$ANGLE_MAC/libGLESv2.dylib" \
        bash "$ROOT/runtime/tools/regress/build-macos-app.sh" "$WORK/macos" | grep -E '^==>|^built'
    [ -d "$WORK/macos/PvZ2 Reflourished.app" ] || { echo "the Mac build failed (see above)" >&2; exit 1; }
    rm -rf "$OUT/PvZ2 Reflourished.app"
    mv "$WORK/macos/PvZ2 Reflourished.app" "$OUT/"

    if [ "$QUICK" = 0 ]; then
        step "packing the Mac app into a disk image"
        bash "$ROOT/scripts/make-dmg.sh" "$OUT/PvZ2 Reflourished.app" "$OUT/PvZ2-Reflourished.dmg"
    fi
fi

step "done"
[ "$WANT_IOS" = 1 ] && echo "    $OUT/PvZ2-Reflourished.ipa"
[ "$WANT_IOS_SIM" = 1 ] && echo "    $OUT/PvZ2-Reflourished-sim.app"
[ "$WANT_MAC" = 1 ] && echo "    $OUT/PvZ2 Reflourished.app"
[ "$WANT_MAC" = 1 ] && [ "$QUICK" = 0 ] && echo "    $OUT/PvZ2-Reflourished.dmg"
exit 0
