# PvZ2: Reflourished — Darwin

**PvZ2: Reflourished on iPhone, iPad and Mac.** No jailbreak, no JIT, no emulator.

The mod's Android code is converted ahead of time into ordinary, signed Apple code,
so the game runs the way any other app does: full screen, at your display's refresh
rate (120 Hz on ProMotion iPads), with sound, saves and the mod's own content built in.

> **Status:** early. It plays well on iPad and Mac; iPhone has not been tested yet.

---

## Get it

You need the **PvZ2 Reflourished 1.4.2** APK from the mod's official downloads. The game's
data (the OBB) comes from the mod's own release by itself.

### Option A — let GitHub build it (no Mac needed)

1. **Fork** this repository.
2. In your fork, open the **Actions** tab and enable workflows.
3. Pick **Build → Run workflow**, paste a link to the APK (a direct link, Google Drive or
   MEGA), and start it. Leave **Slim** on for small apps.
4. When it finishes (up to an hour), the apps are in your fork's **Releases**.

### Option B — build it on your Mac

Needs Xcode (with its iOS platform) and about 8 GB of free space.

```sh
git clone https://github.com/oenderg/Reflourished-Darwin
cd Reflourished-Darwin
./build.sh --apk "PvZ2 Reflourished 1.4.2.apk" --slim
```

The results land in `out/`. Add `--ios` or `--mac` to build just one of them; leave out
`--slim` for apps with the game's data inside (about 1.3 GB more).

---

## Install

**iPhone / iPad** — install `PvZ2-Reflourished.ipa` with SideStore, AltStore,
Sideloadly or LiveContainer. The slim app is about 60 MB: the first time it opens, it
downloads the game's data (1.3 GB) and checks it, so keep it open on Wi-Fi until the game starts.

**AltStore / SideStore source** — add
`https://raw.githubusercontent.com/oenderg/Reflourished-Darwin/main/altstore/source.json`
(Sources → +) and install the game from it; new builds then show up as updates.

**Mac** (Apple silicon) — open `PvZ2-Reflourished.dmg` and drag the app onto
Applications. The app is not notarized, so the first time macOS refuses to open it:
click **Done**, then in **System Settings → Privacy & Security** click **Open Anyway**.
The slim Mac app also downloads the game's data on its first start.

Your saves live with the app (on the Mac, in `~/Library/Application Support/PvZ2 Reflourished`).

## Playing

* **Touch, mouse and trackpad** work as on Android.
* **Keyboard (Mac, and iPad with a keyboard):** `1`–`8` take a seed packet, `S` the
  shovel, `Space` collects every sun and coin on the lawn, `Esc` pauses and resumes. The
  keys rest while the game is paused. On the Mac, Settings (`Cmd-,`) lets you pick any keys.
* **Mac:** the window keeps the game's shape and can be any size (or, in Settings, adapt
  to any shape from 4:3 to 21:9). `Cmd-M` and `Cmd-H` pause the game; closing the window
  quits, or keeps the game paused if you choose that in Settings. Quitting asks first
  (Settings, or "Don't ask again", turns that off). The game is drawn at the sharpness of
  the display it is on; Settings can draw it sharper on a display that isn't Retina.
* Leaving the app pauses the game; coming back shows its pause menu.

## Known issues

* Not tested on iPhone yet.

## How it works

The game's native libraries (`libPVZ2`, the mod's `libRFLib` and their helpers) are
translated from Android's ELF format into Mach-O dylibs at build time, with the mod's
290 code hooks written straight into the converted code. A small host — built on
[Husk](https://github.com/leviidev/husk) — provides what Android would have: the
JNI calls the game makes, its file system, OpenGL ES through ANGLE over Metal, audio,
touch and keyboard input, and the app's lifecycle.

| Folder | What is in it |
|---|---|
| `converter/` | ELF → Mach-O conversion and the hook bake |
| `runtime/` | the host: Android and JNI environment, graphics, audio, input, iOS and Mac apps |
| `data/` | the mod's recorded hook table and the game version it matches |
| `prebuilt/` | ANGLE for iOS (rebuild with `REBUILD_ANGLE=1 ./build.sh …`) |
| `scripts/` | downloads, the disk image, the AltStore source, the Mac test runner |
| `docs/` | how it works, testing, the game's internals, releasing |

More in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Contributing

Fixes and improvements are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) has the build loop and what a
good pull request shows; [docs/TESTING.md](docs/TESTING.md) how to test on a Mac without touching your
own game; [docs/GAME-NOTES.md](docs/GAME-NOTES.md) what the game does that is not obvious. Coding
agents start at [AGENTS.md](AGENTS.md). Found a bug? Open an issue with the app's log.

## Thanks

The **PvZ2 Reflourished** team for the mod, **Husk** for the translation layer this
is built on, and **ANGLE** (WebKit and Chromium) for OpenGL ES on Metal.
