#!/bin/bash
# Build the Mac app for the JIT-free route: the driver, the guest's libraries already
# converted to Mach-O, ANGLE, and the game, in one bundle the user can double-click.
#
#   tools/regress/build-macos-app.sh [out-dir]
#
# Produces $OUT/PvZ2 Reflourished.app. The pieces come from the environment:
#
#   AOT_DYLIB_DIR  the converted guest libraries (scripts/build-macos-dylibs.sh)
#   GAME_APK       the game's APK
#   GAME_OBB       the game's expansion file (the assets); leave it out for a slim app
#   OBB_URLS, OBB_SHA256, OBB_SIZE   where a slim app downloads the expansion file from
#   APP_VERSION, APP_BUILD           the version and build number (default 1.4.2 and 1)
#   GAME_CDN_TAR   the mod's CDN archive (1.4.2.main.tar)
#   ANGLE_EGL      ANGLE's libEGL.dylib   (default: the copy inside Steam's CEF)
#   ANGLE_GLES     ANGLE's libGLESv2.dylib
#
# The bundle is:
#   Contents/MacOS/PvZ2                  the host (tools/pvz2-macos.m) and the driver
#   Contents/Frameworks/*.dylib          the converted guest libraries and their .facts
#   Contents/Frameworks/libEGL.dylib     ANGLE: the EGL the driver needs on a Mac
#   Contents/Game/pvz2.apk               the game's APK
#   Contents/Game/obb/*.obb              the game's expansion file
#   Contents/Game/1.4.2.main.tar         the mod's CDN archive
#
# The writable state is not in the bundle: it is in ~/Library/Application Support/PvZ2
# Reflourished, and the host finds the bundle's pieces relative to its own executable.
set -euo pipefail

R="$(cd "$(dirname "$0")/../.." && pwd)"
N="$R/src/translation-layer-next"
T="$R/src/translation-layer"
OUT="${1:-$R/build/pvz2-macos}"
APP="$OUT/PvZ2 Reflourished.app"
AOT_DYLIB_DIR="${AOT_DYLIB_DIR:-$R/build/macos-dylibs}"

# The guest's libraries, converted ahead of time. Without them the app has no JIT-free
# route: it would translate the guest's ELF, which needs executable memory.
[ -f "$AOT_DYLIB_DIR/libPVZ2.dylib" ] || {
    echo "no converted guest libraries in $AOT_DYLIB_DIR -- run scripts/build-macos-dylibs.sh" >&2
    exit 1
}

CEF="${ANGLE_EGL:-}"
EGL="${ANGLE_EGL:-$CEF/libEGL.dylib}"
GLES="${ANGLE_GLES:-$CEF/libGLESv2.dylib}"
[ -f "$EGL" ] || { echo "no ANGLE at $EGL (set ANGLE_EGL)" >&2; exit 1; }
[ -f "$GLES" ] || { echo "no ANGLE GLES at $GLES (set ANGLE_GLES)" >&2; exit 1; }

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources" "$APP/Contents/Game"

echo "==> compiling the host and the translation layer"
# The same source set as the off-screen harness (tools/regress/build.sh), with the Mac
# host in place of the harness's main().
SRCS="$R/tools/pvz2-macos.m"
for f in "$N"/*.c "$N"/*.m; do case "$f" in *-unity-app.c) ;; *) SRCS="$SRCS $f";; esac; done
SRCS="$SRCS $T/husk-tl-elf.c $T/husk-tl-zip.c $T/husk-tl-sound.c"
# The settings window is SwiftUI: one Swift file, compiled to an object and linked
# with the rest (the Swift runtime is the system's own).
SWIFT_OBJ="$(mktemp -d)/settings.o"
xcrun swiftc -c -parse-as-library -O -target arm64-apple-macos14.0 \
    "$R/tools/pvz2-macos-settings.swift" -o "$SWIFT_OBJ"
SWIFT_LIB="$(dirname "$(xcrun -f swiftc)")/../lib/swift/macosx"
clang -g -O1 -fobjc-arc -Wall -Wno-unused-function -Wno-deprecated-declarations \
    -I"$T" -I"$N" \
    "$SWIFT_OBJ" -L"$SWIFT_LIB" -L/usr/lib/swift -Wl,-rpath,/usr/lib/swift \
    -Wl,-U,_husk_native_loaded_apk \
    -Wl,-rpath,@executable_path/../Frameworks \
    $SRCS \
    -lz -lm -lresolv -lsqlite3 \
    -framework AudioToolbox -framework Foundation -framework CoreFoundation -framework CoreText \
    -framework CoreGraphics -framework Security -framework QuartzCore -framework Metal \
    -framework IOSurface -framework ImageIO -framework WebKit -framework AppKit \
    -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework Accelerate \
    -o "$APP/Contents/MacOS/PvZ2"

echo "==> the converted guest libraries"
for d in "$AOT_DYLIB_DIR"/*.dylib; do
    [ -f "$d" ] || continue
    cp "$d" "$APP/Contents/Frameworks/"
done
# The .facts file beside each dylib is the symbol map: without it the driver cannot ask
# the converted library for a name by name, and the game never starts.
#
# It cannot simply be put there. Everything inside Contents/Frameworks is nested code
# as far as codesign is concerned, and a file that is not signed is a file the bundle
# refuses to seal ("code object is not signed at all"). So the map itself is a
# resource, in Contents/Resources, and the Frameworks directory holds a link to it:
# the driver opens the name the loader knows and the link takes it there.
for d in "$AOT_DYLIB_DIR"/*.facts; do
    [ -f "$d" ] || continue
    name="$(basename "$d")"
    cp "$d" "$APP/Contents/Resources/$name"
    ln -sf "../Resources/$name" "$APP/Contents/Frameworks/$name"
done
ls "$APP/Contents/Frameworks" | sed 's/^/    /'

echo "==> ANGLE"
cp "$EGL" "$APP/Contents/Frameworks/libEGL.dylib"
cp "$GLES" "$APP/Contents/Frameworks/libGLESv2.dylib"
# The guest's libraries were linked against ANGLE's GLES with a relative install name
# ("./libGLESv2.dylib"), which dyld resolves against the directory of the library that
# asks for it -- so it has to sit beside them, which is where it is.

if [ -n "${GAME_APK:-}" ]; then
    [ -f "$GAME_APK" ] || { echo "no game apk at $GAME_APK" >&2; exit 1; }
    cp "$GAME_APK" "$APP/Contents/Game/pvz2.apk"
    echo "==> the game: $(du -h "$GAME_APK" | cut -f1) apk"
fi
if [ -n "${GAME_OBB:-}" ]; then
    [ -f "$GAME_OBB" ] || { echo "no obb at $GAME_OBB" >&2; exit 1; }
    mkdir -p "$APP/Contents/Game/obb"
    # The expansion file is 1.2 GB. On APFS a clone is a copy that costs nothing until
    # it is written to, which is the difference between a bundle the user can copy and
    # one that takes a minute and a second gigabyte.
    cp -c "$GAME_OBB" "$APP/Contents/Game/obb/main.675.com.ea.game.pvz2_rfl.obb" 2>/dev/null \
        || cp "$GAME_OBB" "$APP/Contents/Game/obb/main.675.com.ea.game.pvz2_rfl.obb"
    echo "==> the game: $(du -h "$GAME_OBB" | cut -f1) obb"
fi
if [ -n "${GAME_CDN_TAR:-}" ]; then
    [ -f "$GAME_CDN_TAR" ] || { echo "no cdn tar at $GAME_CDN_TAR" >&2; exit 1; }
    cp "$GAME_CDN_TAR" "$APP/Contents/Game/1.4.2.main.tar"
    echo "==> the CDN archive: $(du -h "$GAME_CDN_TAR" | cut -f1)"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>PvZ2</string>
	<key>CFBundleIdentifier</key><string>org.husk.pvz2.macos</string>
	<key>CFBundleName</key><string>PvZ2 Reflourished</string>
	<key>CFBundleDisplayName</key><string>PvZ2 Reflourished</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleVersion</key><string>${APP_BUILD:-1}</string>
	<key>CFBundleShortVersionString</key><string>${APP_VERSION:-1.4.2}</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST

# Where the app downloads the expansion file from when it is not there (a slim app,
# built without GAME_OBB): the links in order, and the size and sha256 it must have.
if [ -n "${OBB_URLS:-}" ]; then
    [ -n "${OBB_SHA256:-}" ] && [ -n "${OBB_SIZE:-}" ] || { echo "OBB_URLS needs OBB_SHA256 and OBB_SIZE" >&2; exit 1; }
    PB=/usr/libexec/PlistBuddy
    $PB -c "Add :PVZ2ObbURLs array" "$APP/Contents/Info.plist"
    i=0
    for u in $OBB_URLS; do
        $PB -c "Add :PVZ2ObbURLs:$i string $u" "$APP/Contents/Info.plist"; i=$((i + 1))
    done
    $PB -c "Add :PVZ2ObbSHA256 string $OBB_SHA256" "$APP/Contents/Info.plist"
    $PB -c "Add :PVZ2ObbSize integer $OBB_SIZE" "$APP/Contents/Info.plist"
    $PB -c "Add :PVZ2ObbName string main.675.com.ea.game.pvz2_rfl.obb" "$APP/Contents/Info.plist"
    echo "==> the expansion file is downloaded from $i link(s) when it is missing"
fi

# Sign innermost first, then the bundle: a nested library that is not signed with the
# same identity as the app that loads it is refused at load time, and the bundle's own
# signature covers everything inside it.
echo "==> signing"
for d in "$APP/Contents/Frameworks"/*.dylib; do
    [ -f "$d" ] || continue
    codesign --force --timestamp=none --sign - "$d"
done
codesign --force --timestamp=none --sign - "$APP"

echo
echo "built: $APP  ($(du -sh "$APP" | cut -f1))"
echo
cat <<EOF
run it:
  open "$APP"

its state and its log:
  ~/Library/Application\ Support/PvZ2\ Reflourished/

with another data directory, another set of libraries or another game:
  TL_DATA=... TL_AOT_DIR=... TL_PVZ2_APK=... "$APP/Contents/MacOS/PvZ2"
EOF
