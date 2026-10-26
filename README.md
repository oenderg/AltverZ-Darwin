<h1 align="center">AltVerZ-Darwin</h1>

<p align="center"><b>PvZ2: AltVerZ 1.8.3f natively on iPhone, iPad and Mac.</b><br>No jailbreak, no JIT, no emulator.</p>

<p align="center">
The mod's Android code is converted ahead of time into ordinary, signed Apple code, so the game
runs the way any other app does: full screen, at your display's refresh rate, with sound, saves
and the mod's own content built in.
</p>

<p align="center">
  <a href="https://github.com/oenderg/AltverZ-Darwin/releases"><img alt="Latest release" src="https://img.shields.io/github/v/release/oenderg/AltverZ-Darwin?include_prereleases&label=latest&color=8a3ffc"></a>
  <a href="https://github.com/oenderg/AltverZ-Darwin/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/oenderg/AltverZ-Darwin/total?color=e0702c"></a>
  <img alt="Game version" src="https://img.shields.io/badge/PvZ2%20AltVerZ-1.8.3f-2ea44f">
  <img alt="iPhone and iPad" src="https://img.shields.io/badge/iOS%20%26%20iPadOS-15.5%2B-0a84ff?logo=apple&logoColor=white">
  <img alt="Mac" src="https://img.shields.io/badge/macOS-12%2B%20·%20Apple%20silicon-0a84ff?logo=apple&logoColor=white">
  <a href="https://github.com/oenderg/AltverZ-Darwin/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/oenderg/AltverZ-Darwin?style=social"></a>
</p>

<p align="center">
  <a href="https://github.com/oenderg/AltverZ-Darwin/releases"><b>Download</b></a> ·
  <a href="#features">Features</a> ·
  <a href="#install">Install guide</a> ·
  <a href="#build-it-yourself">Build it yourself</a> ·
  <a href="#how-it-works">How it works</a>
</p>

---

> **Early build, expect bugs.**

## Features

* **A real app**
  * Full screen, with sound, saves and the mod's own content built in.
  * Leaving the app pauses the game; coming back shows its pause menu.
* **Unlocked frame rate**
  * The game runs at your display's refresh rate: 120 Hz on ProMotion displays.
  * Turn on **Target 60 FPS** in the game's own settings for that; with it off, the game runs at 60.
* **Touch, mouse and trackpad** work as on Android.
* **Keyboard** (Mac, and iPad with a keyboard)
  * `1`-`8` take a seed packet, `S` the shovel.
  * `Space` collects every sun, coin, gem and plant food drop on the lawn.
  * `F` toggles fast-forward, `Q` picks up plant food, and `Z`, `X`, `C` select the level's
    power-ups from left to right.
  * `Esc` pauses and resumes. The keys rest while the game is paused.
  * On the Mac, Settings (`Cmd-,`) lets you pick any keys.
* **Mac window**
  * Keeps the game's shape at any size, or, in Settings, adapts to any shape from 4:3 to 21:9.
  * Drawn at the sharpness of the display it is on; Settings can draw it sharper on a
    display that isn't Retina.
  * `Cmd-M` and `Cmd-H` pause the game.
* **Updates**
  * The Mac app updates itself: it checks once a day and asks before it installs
    (PvZ2 AltVerZ > Check for Updates…).
  * iPhone and iPad get updates through AltStore and SideStore with this repository's source.
  * Your saves stay, and a full app needs no second 1.1 GB download.

## Install

Every [release](https://github.com/oenderg/AltverZ-Darwin/releases) has two downloads for each
platform:

| File | What it is |
|---|---|
| `PvZ2-AltVerZ-<game>-Darwin-<version>-iOS.ipa` | iPhone and iPad, slim (about 50 MB) |
| `PvZ2-AltVerZ-<game>-Darwin-<version>-iOS-Full.ipa` | iPhone and iPad, with the game's data inside (about 1.1 GB) |
| `PvZ2-AltVerZ-<game>-Darwin-<version>-Mac.dmg` | Mac, slim (about 55 MB) |
| `PvZ2-AltVerZ-<game>-Darwin-<version>-Mac-Full.dmg` | Mac, with the game's data inside |

A slim app downloads the game's data (about 1.1 GB) the first time it opens and checks it, so keep
it open on Wi-Fi until the game starts.

**iPhone / iPad** (iOS 15.5 or later) - install the IPA with SideStore, AltStore, Sideloadly or
LiveContainer.

**AltStore / SideStore source** - add
`https://raw.githubusercontent.com/oenderg/AltverZ-Darwin/main/altstore/source.json`
(Sources → +) and install the game from it; new releases then show up as updates.

**Mac** (Apple silicon, macOS 12 or later) - open the DMG and drag the app onto Applications. The
app is not notarized, so the first time macOS refuses to open it: click **Done**, then in
**System Settings → Privacy & Security** click **Open Anyway**. After that the app updates itself.

Your saves live with the app (on the Mac, in `~/Library/Application Support/PvZ2 AltVerZ`).
AltVerZ has no Import/Export menu, so to bring a save over from Android, copy `pp.dat`,
`pp.dat.bak`, `local_profiles` and `global_save_data` from
`Android/data/com.ea.game.pvz2_auz/files/No_Backup/` on the phone into
`sdcard/Android/data/com.ea.game.pvz2_auz/files/No_Backup/` inside that folder (on iPhone and
iPad, inside **PvZ2 AltVerZ** → `data` in the Files app), with the app closed.

## Build it yourself

You need the **PvZ2 AltVerZ 1.8.3f** APK from the mod's official download. You don't need the
OBB: the build downloads it from the mod's own link by itself and checks it. To use your own
copy instead, pass it with `--obb` (or the workflow's OBB link).

### Option A - let GitHub build it (no Mac needed)

1. **Fork** this repository.
2. In your fork, open the **Actions** tab and enable workflows.
3. Pick **Build → Run workflow**, paste a link to the APK (a direct link, Google Drive or MEGA),
   and start it.
4. When it finishes (up to an hour), the apps are in your fork's **Releases**.

### Option B - build it on your Mac

Needs Xcode (with its iOS platform) and about 8 GB of free space.

```sh
git clone https://github.com/oenderg/AltverZ-Darwin
cd AltverZ-Darwin
./build.sh --profile altverz --apk "AltVerZ_1.8.3f.apk" --slim
```

The results land in `out/`. Add `--ios` or `--mac` to build just one of them; leave out `--slim`
for apps with the game's data inside (about 1.1 GB more).

## Known issues

* Early build: tested on the Mac, and only lightly on iPhone and iPad so far.

## How it works

The game's native libraries (`libPVZ2`, AltVerZ's `libSnowie` and their helpers) are translated
from Android's ELF format into Mach-O dylibs at build time, with the mod's 69 code hooks written
straight into the converted code. A small host - built on
[Husk](https://github.com/leviidev/husk) - provides what Android would have: the JNI calls the game
makes, its file system, OpenGL ES through ANGLE over Metal, audio, touch and keyboard input, and
the app's lifecycle.

| Folder | What is in it |
|---|---|
| `converter/` | ELF → Mach-O conversion and the hook bake |
| `runtime/` | the host: Android and JNI environment, graphics, audio, input, iOS and Mac apps |
| `data/` | the mod's recorded hook table, the game version it matches, and the mod profiles |
| `prebuilt/` | ANGLE for iOS and the iOS Simulator (rebuild with `REBUILD_ANGLE=1 ./build.sh …`) |
| `scripts/` | downloads, the disk image, the AltStore source, the Mac test runner |
| `docs/` | how it works, testing, the game's internals, releasing |

More in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Contributing

Fixes and improvements are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) has the build loop and what a
good pull request shows; [docs/TESTING.md](docs/TESTING.md) how to test on a Mac without touching your
own game; [docs/GAME-NOTES.md](docs/GAME-NOTES.md) what the game does that is not obvious. Coding
agents start at [AGENTS.md](AGENTS.md). Found a bug? Open an issue with the app's log.

## Thanks

The **AltVerZ** team for the mod, **Reflourished-Darwin**, which this port is built on,
**Husk** for the translation layer, and **ANGLE** (WebKit and Chromium) for OpenGL ES on Metal.
