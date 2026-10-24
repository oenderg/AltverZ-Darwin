#!/bin/bash
# Build the minimal iOS host for the PvZ2 driver: tools/pvz2-ios.m plus the whole
# translation layer, presented through a CAMetalLayer, over ANGLE. No QEMU,
# virglrenderer, MoltenVK or guest image -- a game like this needs only ANGLE.
#
#   tools/regress/build-ios-app.sh [out-dir]
#
# Then sign for the device (see the last lines it prints). ANGLE must have been
# built first: scripts/build_angle_ios.sh (it needs the Xcode Metal toolchain:
# xcodebuild -downloadComponent MetalToolchain).
set -euo pipefail

R="$(cd "$(dirname "$0")/../.." && pwd)"
N="$R/src/translation-layer-next"
T="$R/src/translation-layer"
ANGLE="$R/build/ios-arm64/sysroot/lib/libANGLE-shared.dylib"
OUT="${1:-$R/build/pvz2-ios}"
APP="$OUT/PvZ2.app"
DEPLOY=15.5
SDKNAME=iphoneos
TARGET="arm64-apple-ios$DEPLOY"
SDK_ARGS=()
if [ "${IOS_SIM:-0}" = 1 ]; then
    APP="${IOS_SIM_APP:-$OUT/PvZ2-Reflourished-sim.app}"
    ANGLE="$R/build/iossim-arm64/sysroot/lib/libANGLE-shared.dylib"
    SDKNAME=iphonesimulator
    TARGET="$TARGET-simulator"
    SDK_ARGS=(-isysroot "$(xcrun --sdk "$SDKNAME" --show-sdk-path)")
fi
# A specific provisioning profile fixes the bundle ID; override when signing with one.
BUNDLE_ID="${BUNDLE_ID:-io.github.oenderg.reflourished-darwin}"

[ -f "$ANGLE" ] || { echo "no ANGLE at $ANGLE -- run scripts/build_angle_ios.sh first" >&2; exit 1; }

rm -rf "$APP"
mkdir -p "$APP/Frameworks"
# Same source set as the macOS harness (tools/regress/build.sh), minus the app
# glue the harness does not use, plus the iOS JIT helper.
. "$R/tools/regress/pvz2-sources.sh"
SRCS="$R/tools/pvz2-ios.m $(pvz2_runtime_sources | tr '\n' ' ')"
[ "${AOT_DYLIB_TEST:-0}" = 1 ] && SRCS="$SRCS -DAOT_DYLIB_TEST=1"

echo "==> compiling for $TARGET (this is the whole translation layer)"
# -U: the console host asks the app glue which game is loaded; this host has none.
xcrun --sdk "$SDKNAME" clang -target "$TARGET" ${SDK_ARGS[@]+"${SDK_ARGS[@]}"} -O1 -g -fobjc-arc \
    -Wall -Wno-unused-function -Wno-deprecated-declarations \
    -I"$T" -I"$N" \
    -Wl,-U,_husk_native_loaded_apk -Wl,-rpath,@executable_path/Frameworks \
    $SRCS \
    -framework UIKit -framework QuartzCore -framework Metal -framework Foundation \
    -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework Security \
    -framework IOSurface -framework ImageIO -framework AudioToolbox -framework AVFAudio -framework VideoToolbox \
    -framework CoreMedia -framework CoreVideo -framework Accelerate -framework WebKit \
    -framework CoreMotion \
    -lz -lm -lresolv -lsqlite3 \
    -o "$APP/PvZ2"

cp "$ANGLE" "$APP/Frameworks/"

# BEGIN JIT-FREE GUEST LIBRARIES
# The guest libraries converted ahead of time (scripts/build-ios-dylibs.sh): arm64 Mach-O
# dylibs for iOS with every load-time rewrite baked in. With them in the bundle the
# translation layer has nothing to translate and nothing to make executable -- dyld loads
# them like any other dylib -- which is what a device that enforces TXM asks for.
#
# Set AOT_DYLIB_DIR to the directory scripts/build-ios-dylibs.sh wrote. If it is absent
# the app falls back to translating the guest's ELF, which needs a debugger attached.
AOT_DYLIB_DIR="${AOT_DYLIB_DIR:-$(cd "$R/../../.." 2>/dev/null && pwd || echo /nonexistent)/research/build/ios-dylibs}"
if [ -f "$AOT_DYLIB_DIR/libPVZ2.dylib" ]; then
    for d in "$AOT_DYLIB_DIR"/*.dylib "$AOT_DYLIB_DIR"/*.facts; do
        [ -f "$d" ] || continue
        cp "$d" "$APP/Frameworks/"
    done
    echo "==> staged the converted guest libraries ($(du -sh "$AOT_DYLIB_DIR" | cut -f1)):" \
         "$(cd "$AOT_DYLIB_DIR" && ls *.dylib | tr '\n' ' ')"
else
    echo "==> no converted libraries at $AOT_DYLIB_DIR; the app will translate the guest's"
    echo "    ELF instead, which needs StikDebug attached at launch (run scripts/build-ios-dylibs.sh)"
fi
# END JIT-FREE GUEST LIBRARIES

if [ "${IOS_SIM:-0}" = 1 ]; then
    # Apple silicon runs the same arm64 instructions, but dyld requires the simulator
    # platform on every image. Only bundle copies change; device inputs stay intact.
    sdk_version="$(xcrun --sdk "$SDKNAME" --show-sdk-version)"
    for dylib in "$APP"/Frameworks/*.dylib; do
        echo "==> relabelling $(basename "$dylib") for the iOS Simulator"
        xcrun vtool -set-build-version iossim "$DEPLOY" "$sdk_version" -replace \
            -output "$dylib.simulator" "$dylib"
        mv -f "$dylib.simulator" "$dylib"
    done
fi

[ -d "$OUT/PvZ2.dSYM" ] || true

cat > "$APP/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>PvZ2</string>
	<key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
	<key>CFBundleName</key><string>PvZ2 Reflourished</string>
	<key>CFBundleDisplayName</key><string>PvZ2 Reflourished</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleVersion</key><string>${APP_BUILD:-1}</string>
	<key>CFBundleShortVersionString</key><string>${APP_VERSION:-1.4.2}</string>
	<key>MinimumOSVersion</key><string>$DEPLOY</string>
	<key>UIRequiredDeviceCapabilities</key><array><string>arm64</string></array>
	<!-- Without this iOS treats the app as iPhone-only and runs it in a compatibility
	     window on iPad: letterboxed, portrait, a fraction of the screen. -->
	<key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
	<!-- The phone: landscape only, as phone games are. A landscape strip on a
	     portrait phone is a third of the screen. -->
	<key>UISupportedInterfaceOrientations</key>
	<array>
		<string>UIInterfaceOrientationLandscapeLeft</string>
		<string>UIInterfaceOrientationLandscapeRight</string>
	</array>
	<!-- The iPad reads this key, not the one above. The user wants the app to
	     rotate like other games do: in portrait the window is portrait, and
	     the game is a landscape strip in it, with bars above and below. The
	     game itself never gets a portrait layout. -->
	<key>UISupportedInterfaceOrientations~ipad</key>
	<array>
		<string>UIInterfaceOrientationPortrait</string>
		<string>UIInterfaceOrientationPortraitUpsideDown</string>
		<string>UIInterfaceOrientationLandscapeLeft</string>
		<string>UIInterfaceOrientationLandscapeRight</string>
	</array>
	<key>UIStatusBarHidden</key><true/>
	<key>UIViewControllerBasedStatusBarAppearance</key><false/>
	<!-- No storyboard: an empty UILaunchScreen opts into native full-screen (and
	     landscape) instead of compatibility letterboxing. -->
	<key>UILaunchScreen</key><dict/>
	<key>UIRequiresFullScreen</key><true/>
	<!-- iOS 26+ kills an app that has not adopted the scene lifecycle. -->
	<key>UIApplicationSceneManifest</key>
	<dict>
		<key>UIApplicationSupportsMultipleScenes</key><false/>
		<key>UISceneConfigurations</key>
		<dict>
			<key>UIWindowSceneSessionRoleApplication</key>
			<array>
				<dict>
					<key>UISceneConfigurationName</key><string>Default</string>
					<key>UISceneDelegateClassName</key><string>PVZ2SceneDelegate</string>
				</dict>
			</array>
		</dict>
	</dict>
	<!-- The APK and OBB are put in Documents from Files/Finder. -->
	<key>UIFileSharingEnabled</key><true/>
	<key>LSSupportsOpeningDocumentsInPlace</key><true/>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

if [ "${IOS_SIM:-0}" = 1 ]; then
    /usr/libexec/PlistBuddy -c 'Add :CFBundleSupportedPlatforms array' "$APP/Info.plist"
    /usr/libexec/PlistBuddy -c 'Add :CFBundleSupportedPlatforms:0 string iPhoneSimulator' "$APP/Info.plist"
fi

# ---------------------------------------------------------------- game data and icon
# Both are optional and both happen before signing, because anything added to the bundle
# invalidates the signature and the bundle has to be sealed exactly once.
if [ -n "${GAME_APK:-}" ]; then
    [ -f "$GAME_APK" ] || { echo "no game apk at $GAME_APK" >&2; exit 1; }
    GAME_OBB="${GAME_OBB:-/tmp/1.4.2.main.tar}"
    mkdir -p "$APP/Game"
    cp "$GAME_APK" "$APP/Game/pvz2.apk"
    if [ "$GAME_OBB" = none ]; then
        # the slim app: the host downloads the expansion file on the first launch
        # (OBB_URLS below), so the bundle carries the apk only
        echo "==> bundled the game: $(du -h "$GAME_APK" | cut -f1) apk, no obb (it is downloaded on the first launch)"
    else
        [ -f "$GAME_OBB" ] || { echo "no obb at $GAME_OBB" >&2; exit 1; }
        mkdir -p "$APP/Game/obb"
        # the host finds the expansion file by its extension, and the game's data is exactly
        # what the expansion file holds, so the name is all that matters here
        cp "$GAME_OBB" "$APP/Game/obb/main.675.com.ea.game.pvz2_rfl.obb"
        echo "==> bundled the game: $(du -h "$GAME_APK" | cut -f1) apk, $(du -h "$GAME_OBB" | cut -f1) obb"
    fi
fi
# Where the host downloads the expansion file from when it is not there (a slim app, or a
# player who deleted it): the links in order, and the size and sha256 it must have.
if [ -n "${OBB_URLS:-}" ]; then
    [ -n "${OBB_SHA256:-}" ] && [ -n "${OBB_SIZE:-}" ] || { echo "OBB_URLS needs OBB_SHA256 and OBB_SIZE" >&2; exit 1; }
    PB=/usr/libexec/PlistBuddy
    $PB -c "Add :PVZ2ObbURLs array" "$APP/Info.plist"
    i=0
    for u in $OBB_URLS; do
        $PB -c "Add :PVZ2ObbURLs:$i string $u" "$APP/Info.plist"; i=$((i + 1))
    done
    $PB -c "Add :PVZ2ObbSHA256 string $OBB_SHA256" "$APP/Info.plist"
    $PB -c "Add :PVZ2ObbSize integer $OBB_SIZE" "$APP/Info.plist"
    $PB -c "Add :PVZ2ObbName string main.675.com.ea.game.pvz2_rfl.obb" "$APP/Info.plist"
    echo "==> the expansion file is downloaded from $i link(s) when it is missing"
fi
# The mod's CDN content. The game asks for it over the network, and a device that is
# not on a network gets nothing -- so the archive goes into the bundle and the host
# stages it where the translation layer looks for it (the data directory's No_Backup).
if [ -n "${GAME_CDN_TAR:-}" ]; then
    [ -f "$GAME_CDN_TAR" ] || { echo "no cdn tar at $GAME_CDN_TAR" >&2; exit 1; }
    mkdir -p "$APP/Game"
    cp "$GAME_CDN_TAR" "$APP/Game/1.4.2.main.tar"
    echo "==> bundled the CDN archive: $(du -h "$GAME_CDN_TAR" | cut -f1)"
fi
if [ -n "${ICON_PNG:-}" ]; then
    [ -f "$ICON_PNG" ] || { echo "no icon at $ICON_PNG" >&2; exit 1; }
    # iOS refuses an icon with an alpha channel, and the game's png has one
    flat="$OUT/icon-flat.jpg"
    sips -s format jpeg "$ICON_PNG" --out "$flat" >/dev/null 2>&1
    for pair in "120 AppIcon60x60@2x" "152 AppIcon76x76@2x" "167 AppIcon83.5x83.5@2x" \
                "180 AppIcon60x60@3x"; do
        set -- $pair
        sips -z "$1" "$1" "$flat" --out "$APP/$2.png" >/dev/null 2>&1
    done
    rm -f "$flat"
    PB=/usr/libexec/PlistBuddy
    $PB -c "Add :CFBundleIcons dict" "$APP/Info.plist" 2>/dev/null || \
        $PB -c "Set :CFBundleIcons dict" "$APP/Info.plist"
    $PB -c "Add :CFBundleIcons:CFBundlePrimaryIcon dict" "$APP/Info.plist"
    $PB -c "Add :CFBundleIcons:CFBundlePrimaryIcon:CFBundleIconFiles array" "$APP/Info.plist"
    for n in AppIcon60x60 AppIcon76x76 AppIcon83.5x83.5; do
        $PB -c "Add :CFBundleIcons:CFBundlePrimaryIcon:CFBundleIconFiles: string $n" \
            "$APP/Info.plist"
    done
    echo "==> set Reflourished's icon"
fi


# Ad-hoc by default: enough to check the build and its load commands locally.
# A stock device needs a real identity. With SIGN_PROFILE the profile's own
# entitlements are used and the profile is embedded -- a signing identity cannot
# grant entitlements its profile does not list (that is what makes an ad-hoc
# distribution profile refuse get-task-allow, and with it the JIT).
IDENTITY="${SIGN_IDENTITY:-}"
ENTITLEMENTS="$R/tools/pvz2-ios.entitlements"
if [ "${IOS_SIM:-0}" = 1 ]; then
    # Simulator code needs no development identity and no entitlements at all: an ad-hoc
    # signature that carries get-task-allow is refused by the Mac ("Security policy issue").
    IDENTITY="-"
    ENTITLEMENTS=""
    echo "==> ad-hoc signing for the iOS Simulator (no provisioning profile)"
else
    if [ -n "${SIGN_PROFILE:-}" ]; then
        [ -f "$SIGN_PROFILE" ] || { echo "no profile at $SIGN_PROFILE" >&2; exit 1; }
        security cms -D -i "$SIGN_PROFILE" > "$OUT/profile.raw.plist"
        plutil -extract Entitlements xml1 -o "$OUT/profile.entitlements" "$OUT/profile.raw.plist"
        ENTITLEMENTS="$OUT/profile.entitlements"
        cp "$SIGN_PROFILE" "$APP/embedded.mobileprovision"
        echo "==> embedding $(basename "$SIGN_PROFILE"); signing entitlements come from it"
    fi
    if [ -n "$IDENTITY" ]; then
        echo "==> signing with $IDENTITY"
    else
        echo "==> ad-hoc signing (set SIGN_IDENTITY/SIGN_PROFILE for the device)"
        IDENTITY="-"
    fi
fi
KEYCHAIN_ARG=()
[ "${IOS_SIM:-0}" != 1 ] && [ -n "${SIGN_KEYCHAIN:-}" ] && KEYCHAIN_ARG=(--keychain "$SIGN_KEYCHAIN")
ENT_ARG=()
[ -n "$ENTITLEMENTS" ] && ENT_ARG=(--entitlements "$ENTITLEMENTS")
# BEGIN AOT-DYLIB-LOADER TEST (sign every nested dylib, not just ANGLE)
# iOS library validation requires the main binary's Team ID on every piece of code the
# process loads, so the converted guest dylib has to be signed with the same identity
# as the app. Sign innermost-first, then the bundle, so the sealed resources cover it.
#
# The "+" form of the expansion is the one that works here: expanding an empty array
# with "${a[@]}" is an unbound-variable error under `set -u` in bash 3.2, the shell
# macOS ships, and SIGN_KEYCHAIN is unset by default.
for dylib in "$APP"/Frameworks/*.dylib; do
    [ -f "$dylib" ] || continue
    echo "==> signing $(basename "$dylib")"
    codesign --force --timestamp=none ${KEYCHAIN_ARG[@]+"${KEYCHAIN_ARG[@]}"} \
        --sign "$IDENTITY" ${ENT_ARG[@]+"${ENT_ARG[@]}"} "$dylib"
done
# END AOT-DYLIB-LOADER TEST
# The dSYM is written next to the binary, which lands it inside the bundle. It has to
# come out *before* the bundle is signed: codesign seals everything under the bundle,
# the dSYM included, so moving it afterwards leaves the seal referring to files that
# are no longer there and the bundle refuses to install.
if [ -d "$APP/PvZ2.dSYM" ]; then rm -rf "$OUT/PvZ2.dSYM"; mv "$APP/PvZ2.dSYM" "$OUT/"; fi
codesign --force --timestamp=none ${KEYCHAIN_ARG[@]+"${KEYCHAIN_ARG[@]}"} \
    --sign "$IDENTITY" ${ENT_ARG[@]+"${ENT_ARG[@]}"} "$APP"

echo
echo "built: $APP  ($(du -sh "$APP" | cut -f1))"

if [ "${IOS_SIM:-0}" = 1 ]; then
    exit 0
fi

# A plain IPA for sideloaders (SideStore/AltStore/StikStore sign it themselves and
# supply the provisioning profile, which is why we do not need the Apple ID here).
IPA="$OUT/PvZ2.ipa"
rm -rf "$OUT/Payload" "$IPA"
mkdir -p "$OUT/Payload"
cp -R "$APP" "$OUT/Payload/"
( cd "$OUT" && zip -qry "$IPA" Payload && rm -rf Payload )
echo "ipa:   $IPA  ($(du -sh "$IPA" | cut -f1))"

otool -L "$APP/PvZ2" | sed -n '2p'
echo
cat <<EOF
next, for a stock device (needs a development profile with get-task-allow):
  1. sideload the IPA (SideStore/AltStore/StikStore sign it and supply the profile),
     or sign the .app yourself and install it:
       SIGN_IDENTITY="Apple Development: you@example.com (TEAMID)" $0 "$OUT"
       xcrun devicectl device install app --device <udid> "$APP"
  2. put the game files in once the app is installed (Files app, or Finder):
       Documents/pvz2.apk
       Documents/obb/main.675.com.ea.game.pvz2_rfl.obb
  3. attach StikDebug (it must be attached at launch, before the game starts,
     or there is no executable memory and the app logs "JIT prewarm FAILED").
  4. read Documents/pvz2.log from Files for the runtime log.
EOF
