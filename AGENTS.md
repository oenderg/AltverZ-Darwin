# AGENTS.md

Orientation for AI coding agents (and people in a hurry). Read this, then the doc for the
area you touch. Measure before you claim: nothing here "works" until a frame, a log line or
a screenshot shows it.

## What this repo is

PvZ2: AltVerZ 1.8.3f (an Android game mod) running natively on iPhone, iPad and Mac with no
JIT. The game's Android libraries are converted **ahead of time** into signed Mach-O libraries
(`converter/`), and a small runtime (`runtime/`, derived from Husk) provides the Android
environment they expect. This repository is based on
[Reflourished-Darwin](https://github.com/oenderg/Reflourished-Darwin); the same runtime builds
either mod from a profile. Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

```
build.sh                      the one entry point: APK -> IPA / Mac app / dmg (./build.sh --help)
converter/                    ELF .so -> signed Mach-O dylib, with the mod's hooks baked in
VERSION                       Reflourished's app version (the altverz profile carries its own)
data/game-1.8.3f.json         the supported AltVerZ build: hashes, OBB links, CDN archive
data/game-1.4.2.json          Reflourished's build, for the comparison profile
data/hooks-snowie-1.8.3f.json AltVerZ's 69 inline hooks, recorded once for that build
data/hooks-1.4.2.json         Reflourished's 290 hooks
data/profiles/                one mod each: package, app identity, mod library, hooks, CDN, feed
runtime/src/translation-layer-next/   the runtime (bionic libc, JNI, EGL/GLES, audio, loader)
  husk-tl-pvz2.c              THE GAME DRIVER: Java contract, lifecycle, GL loop, PvZ2 features
runtime/tools/pvz2-macos.m    Mac host (window, input, menus, OBB download, updater)
runtime/tools/pvz2-macos-settings.swift   Mac settings window (SwiftUI)
runtime/tools/macos-icon.swift  the Mac app icon from the APK's (a Mac icon's shape)
runtime/tools/pvz2-ios.m      iPhone/iPad host (UIKit, touch, hardware keyboard, OBB download)
runtime/tools/pvz2-keys.h     the shared keyboard mapping
runtime/tools/regress/        the app build scripts; pvz2-sources.sh = which runtime files are built
prebuilt/ios/                 ANGLE for iOS (rebuild: runtime/scripts/build_angle_ios.sh)
prebuilt/ios-simulator/       ANGLE for the iOS Simulator (ANGLE_SDK=iphonesimulator, same script)
scripts/                      downloads, dmg, AltStore source, Mac update feed; .github/workflows/build.yml = CI build
scripts/dmg/                  the Mac DMG window: dmg.html, render.sh -> background.tiff, dmgbuild settings.py
release-notes/                 each release's What's new (<version>.md) and the fixed footer; scripts/release-notes.py
sparkle/                      the Mac update feed (appcast.xml, written by CI) and the update key's public half
docs/                         architecture, game notes, testing, releasing
```

## Build and run

```sh
./build.sh --profile altverz --mac --apk "AltVerZ_1.8.3f.apk"           # first build: ~1-2 min
./build.sh --profile altverz --mac --quick --apk "AltVerZ_1.8.3f.apk"   # after editing runtime/: reuses the converted code
```

The app lands in `out/`. `--profile reflourished` builds the comparison mod with its own APK and
game manifest. The profile names the app, bundle id, data folder, feed URL and release files;
[data/profiles/altverz.json](data/profiles/altverz.json) is the one this repository publishes.
Run the app from the command line with its own data folder and a scripted tap sequence, without
touching your own game data: [docs/TESTING.md](docs/TESTING.md).
You need the game's APK (the user's own file; never commit it, or the OBB, or anything from it).

## Rules

- **Never commit game files** (APK, OBB, `.so`, extracted assets, saves) or personal data
  (paths under your home folder, device IDs, signing identities).
- **Never change the app IDs** (`io.github.oenderg.altverz-darwin` for AltVerZ,
  `io.github.oenderg.reflourished-darwin` for the comparison profile): players' saves and
  settings hang off them.
- **Never change `sparkle/public-key`**: installed Mac apps accept only updates signed with its
  private half ([docs/RELEASING.md](docs/RELEASING.md)). Each profile has its own feed; an
  AltVerZ build must never read Reflourished's, and the reverse.
- **The supported game build is exact.** `build.sh` refuses any other `libPVZ2.so` and
  `libSnowie.so`. Every game address in the driver (`GUEST_*`) is valid for 1.8.3f only:
  [docs/GAME-NOTES.md](docs/GAME-NOTES.md), and for a new version
  [docs/NEW-GAME-VERSION.md](docs/NEW-GAME-VERSION.md).
- **Do not resize the game while it starts.** A new screen size before `tl_pvz2_started()` crashes
  it seconds later; the hosts hold the size until then (GAME-NOTES.md, "Start-up").
- **Call into the game only on its own thread** (the GL thread in `husk-tl-pvz2.c`): hosts post
  requests (`tl_pvz2_request_*`) and the driver performs them between frames.
- Mac tests must not disturb the person using the Mac: no global input events, no stealing focus,
  capture your test window only (TESTING.md). Use a test `BUNDLE_ID` for settings writes;
  never change the real defaults domains. Background launches skip the old-settings import
  unless `TL_PVZ2_IMPORT_FROM` names a test source domain.
- The app version comes from the profile (`altverz`: 0.1.0, its own line); `VERSION` is the
  Reflourished fallback. The game version comes from `data/game-1.8.3f.json`. Build numbers are
  `major*10000 + minor*100 + patch`; slim and full share a number. Release names and the
  prerelease workflow are in [docs/RELEASING.md](docs/RELEASING.md).
- Commit titles: Conventional Commits (`fix(mac): ...`, `feat(keys): ...`, `docs: ...`).
- Comments in this code are full sentences that say *why*; match that style.

## Before you open a pull request

Build both apps (`./build.sh --profile altverz --apk ...`) and show the change working: a
screenshot or a log line from the Mac app, and from an iPhone/iPad if the change touches iOS. See
[CONTRIBUTING.md](CONTRIBUTING.md).
