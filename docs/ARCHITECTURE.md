# Architecture

How an Android game ends up running as an ordinary iPhone, iPad or Mac app, with no JIT.

```
the APK's lib/arm64-v8a/*.so ──converter/──► signed Mach-O dylibs (+ .facts symbol maps)
                                                   │
the host app (pvz2-macos.m / pvz2-ios.m) ──► runtime/ (loader, bionic, JNI, EGL/GLES, audio)
                                                   │
                                    husk-tl-pvz2.c (the game driver) ──► the game
```

## Why ahead of time

iOS (and the App Store's rules) do not let an app write executable code at run time without a
debugger attached. The game's code is ARM64 already, so it does not need translating, only
*relinking*: everything a run-time loader would patch is patched once, on the Mac, and the result
is signed like any other library. On the device nothing writes code.

## The converter (`converter/`)

`build-macos-dylibs.sh` / `build-ios-dylibs.sh` run, per library (`libPVZ2`, `libRFLib`, `libNimble`,
`libc++_shared`), `aot-convert.py`, which:

- keeps the code at the offsets the ELF put it, so every PC-relative branch and load still works;
  the data segment moves (Mach-O wants it page-aligned), so PC-relative references into data are
  re-encoded, and the tool refuses a library where it cannot. The data blob has a segment of its own
  (`__GUESTDATA`), with nothing in front of it, and the converted code references no imports through
  the linker's table (`__got`): linkers differ in what they put first in `__DATA` (Xcode 26's put
  `__got` there and broke the page alignment);
- turns ELF relocations into assembler expressions, so `ld64` writes the rebase/bind information;
- rewrites what Darwin forbids or lays out differently: thread-pointer reads (`mrs tpidr_el0`) point
  at a fake thread block, and uses of register x18 (reserved on Apple platforms) go through stubs;
- bakes in the mod's **290 inline hooks** (`data/hooks-1.4.2.json`). On Android, `libRFLib` patches
  `libPVZ2`'s function prologues at start-up; here those branches are written into the dylib ahead
  of time, and the mod's hook API becomes a lookup. Without the table the game never draws a frame.
- writes a `.facts` file (the guest's symbol names, which live inside the image as bytes) and a
  manifest that `check-build.py` verifies after the build.

`aot-mkshim.py` and `bionic-shim.c` build `libbionic-shim.dylib`: the C library, OpenSL ES, OpenGL ES
and vendor functions the guest imports that Darwin does not have under the same name. Where Darwin
has the same function, the guest binds to libSystem directly.

`aot-hooks.py` turns a recorded log of the mod's code writes into the hook table
(see [NEW-GAME-VERSION.md](NEW-GAME-VERSION.md)).

## The runtime (`runtime/src/`)

Derived from Husk, a general Android translation layer; this repo carries only what PvZ2 needs
(`runtime/tools/regress/pvz2-sources.sh` lists what is left out, `runtime/tools/pvz2-standins.c`
answers the few calls into those parts). The pieces that matter here:

| File(s) | Role |
|---|---|
| `husk-tl-ld.c`, `husk-tl-aot.c` | load the converted dylibs (`TL_AOT_DIR`), resolve names via `.facts`, run constructors |
| `husk-tl-bionic*.c` | Android's libc where it differs from Darwin's: files and paths (`/sdcard/...` mapped into the data folder), threads, `sysconf`, `/proc` files |
| `husk-tl-jni*.c` | JNI with no Java VM: classes and objects are C; Java methods the game calls are answered by tables of C functions |
| `husk-tl-egl.c` | EGL and OpenGL ES on ANGLE (over Metal); fixes for what ANGLE and the game disagree on |
| `husk-tl-audio.c`, `husk-tl-opensles.c` | OpenSL ES audio to the speakers |
| `husk-tl-pvz2.c` | **the game driver** (below) |

## The driver (`husk-tl-pvz2.c`)

- **The Java contract.** The game's Java side (`AndroidGameApp`, `AndroidSurfaceView`, Nimble, ...)
  does not exist; the driver answers the ~130 methods the native code calls (config storage,
  screen size, HTTP, assets, broadcasts) with C functions (`M(APP, "Config_ConfigKeyExists", ...)`).
- **Live content.** `husk-tl-cdn.m` uses Foundation on both platforms to fetch only the
  Reflourished HTTPS host (30 s without data, 60 s in total). It queues real response headers
  (including the server Date) and unchanged gzip bytes for the driver's GL-thread
  callbacks. A validated download updates the writable CDN cache atomically; failures
  use that cache or the staged bundle with device-clock time. See GAME-NOTES.md.
- **Boot order** (the game is written against Android's): libraries -> constructors -> `JNI_OnLoad`
  -> `libRFLib` init -> Nimble launch -> `Native_GameAppInitialize` -> lifecycle observer ->
  will/did finish launching -> foreground -> active.
- **The GL thread** runs the game: creates the EGL context, calls `Native_onSurfaceCreated` /
  `Native_onSurfaceChanged` / `Native_onDrawFrame`, pumps the game's message queue, and between
  frames performs what the hosts asked for: lifecycle changes, size changes, keyboard features.
  **Only this thread calls into the game.**
- **Lifecycle.** Hiding, minimizing, closing (when kept), a sleeping display or the iOS app
  switcher send the game Android's background sequence (it saves then) and stop drawing; coming
  back sends the foreground sequence. The game shows its own pause menu after that.
- **Reminders.** `AndroidNotification` crosses a host hook: the shared C driver stores Android's
  per-ID `userInfo` list and typed Nimble values in the app data folder, while
  `pvz2-notifications.h` schedules through UserNotifications on iPhone and iPad. The permission
  request comes when the game creates its notification channels at launch (as on Android 13);
  tapping a reminder makes its `data.fields` available to the game's intent getter. The Mac host
  installs no hooks on purpose (reminders are a phone habit): it never asks and never schedules,
  and the driver keeps the data as Android does when notifications are not allowed.
- **Input.** Touches and text go into the game's own input queue (`UI_ProcessEvents`), as Android's
  view would deliver them. The keyboard features call the game's own functions instead of faking
  taps: seed packet pick, shovel, collect suns and coins, pause/resume; each is refused unless a
  level is live and nothing covers it. See [GAME-NOTES.md](GAME-NOTES.md).
- **Screen size.** The host gives the driver the surface size; the driver tells the game
  (`Native_onSurfaceChanged`) and writes the size into the game's app object so its touch mapping
  follows. Not during start-up: [GAME-NOTES.md](GAME-NOTES.md), "Start-up".

## The hosts (`runtime/tools/`)

- `pvz2-macos.m`: the Mac app. A window with a `CAMetalLayer`; mouse = touch; keyboard shortcuts
  (`pvz2-keys.h`); menus; the window modes (Fixed: one shape, any size; Adapt: the game lays out for
  the window's shape); the display's scale (the game always starts at least 804 px tall so it picks
  its HD art); the close/quit behaviour; the first-launch OBB download for slim builds; a scripted
  test mode ([TESTING.md](TESTING.md)). Settings UI: `pvz2-macos-settings.swift` (SwiftUI). Icon: the APK's, cut to a Mac icon's shape by
  `macos-icon.swift` at build time.
- `pvz2-ios.m`: the iPhone/iPad app. UIKit scene, touch, hardware-keyboard shortcuts, the OBB
  download, files under `Documents/` (visible in the Files app).
- `regress/build-macos-app.sh`, `regress/build-ios-app.sh`, `regress/build-ios-ipa.sh`: compile the
  runtime + host and assemble the bundle (converted dylibs, ANGLE, the APK, the CDN archive, the
  OBB unless slim). The CDN archive in the bundle is an offline fallback; startup staging
  preserves newer downloaded content in the data folder. `build.sh` at the top drives all of it.

## Data on disk

- Mac: the data folder is `~/Library/Application Support/PvZ2 Reflourished/` (or `TL_DATA`), with
  `pvz2.log` in it. iOS: `Documents/data/`, with `Documents/pvz2.log` and the OBB in `Documents/obb/`.
  Each launch starts a new `pvz2.log` and renames the old one to `pvz2.previous.log` (on the Mac, not
  for a restart to fit the window, nor for a test's `TL_PVZ2_LOG`).
- In the data folder: `config/pvz2-config` (the game's own settings),
  `sdcard/Android/obb/com.ea.game.pvz2_rfl/` (the Mac's OBB),
  `sdcard/Android/data/com.ea.game.pvz2_rfl/files/No_Backup/` (saves, CDN content).
  The Mac host's own settings are in the app's user defaults.
