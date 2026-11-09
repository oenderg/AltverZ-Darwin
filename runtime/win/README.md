# Windows (ARM64)

The runtime built for Windows on ARM: the game's own ARM64 code runs natively, loaded at run
time by the runtime's ELF loader (`husk-tl-ld.c`), with no conversion step. Work in progress.

## What is different from the Darwin build

- **Loading.** Windows lets a process run memory it wrote, so the libraries are loaded from the
  APK at start-up into one read-write-execute region (`husk-tl-xmem.c`), as Husk does with a JIT.
  The mod installs its hooks itself, as on Android.
- **x18.** Windows keeps the thread's TEB in x18. The loader rewrites every guest instruction that
  names x18 to use a "virtual x18" in a TEB TLS slot (the same rewrite the Darwin loader does, with
  the slot found through x18 instead of `tpidrro_el0`).
- **Caches.** User code may not run `dc cvau`/`ic ivau` on Windows: the guest's reads of CTR_EL0
  answer "coherent", and the host flushes with `FlushInstructionCache`.
- **The C library** (`husk-tl-bionic-win*.c`): written for Windows' LLP64 ABI from the four
  libraries' import list. `long` is 32 bits and `wchar_t` 16 on Windows, so nothing whose
  signature or struct uses them is passed through; stat, dirent, tm, timespec, FILE, the pthread
  objects, jmp_buf, z_stream, errno values and open() flags are all converted.
- **Assembly** in the shared files spells symbols with `TL_ASM_NAME` (no underscore on COFF).

## The game program (`PvZ2 Reflourished.exe`)

`runtime/win/build-win.sh game` builds the installed program from the shared driver, the Windows
host (`runtime/tools/pvz2-windows.c`), the settings window
(`runtime/tools/pvz2-windows-settings.c`) and the updater glue
(`runtime/tools/pvz2-windows-updates.c`). `runtime/win/build-win.sh game-portable` builds the
zip's program: the same sources with no updater at all. The installed one links WinSparkle
(`prebuilt/winsparkle/arm64`, fetched by `runtime/win/fetch-winsparkle.sh`) and needs
`WinSparkle.dll` beside it; the portable one neither links nor ships it. Both need `libEGL.dll`
and `libGLESv2.dll` (ANGLE) beside them, and `Game/pvz2.apk` (with `Game/1.4.2.main.tar` when
the APK is the slim one).

The x64 program (`runtime/win/x64/build-x64.sh`) takes the same `game`/`game-portable` targets
and links `prebuilt/winsparkle/x64` for the installed one.

## ANGLE

The program draws through ANGLE over Direct3D 11: `libEGL.dll` and `libGLESv2.dll` beside it,
and `d3dcompiler_47.dll`, which ANGLE's D3D11 backend loads to compile shaders. The zips carry
our own build of ANGLE, for x64 and ARM64, from one pinned commit (`runtime/win/ANGLE_COMMIT`,
the commit Chrome 158.0.8127.18 builds ANGLE from).

`runtime/win/build-angle.cmd` builds it from a prompt on a Windows PC with Visual Studio (the
C++ workload for x64 and ARM64) and the Windows SDK the commit's build names:

```bat
runtime\win\build-angle.cmd             both architectures
runtime\win\build-angle.cmd arm64       one of them, x64 or arm64
set ANGLE_COMMIT=<sha> & runtime\win\build-angle.cmd
```

It fetches depot_tools and ANGLE into `out\win\angle-build`, drops the DEPS entries a D3D11
renderer does not need, runs `gclient sync`, `gn gen` with the Vulkan, desktop GL and WGPU
backends off, then `autoninja libEGL libGLESv2`, and leaves the DLLs, `commit.txt`,
`gn-args.txt` and ANGLE's `LICENSE` in `prebuilt\win-<arch>`.

`.github/workflows/angle-windows.yml` runs that script on `windows-2025` for both architectures
at once and uploads one artifact per architecture. Those builds live as zips in the private
draft release `build-inputs`, not in git; `runtime/win/fetch-angle.sh` downloads and checks them
into `prebuilt/win-*`, and the READMEs there name the run. docs/windows-shipping.md has the pinned commit,
the build arguments and how to refresh them.

## Dark mode

The host's own windows follow Windows' light or dark mode: the Settings window, the game data
download window, the x64 preparing window, and the game window's title bar. The game's picture
is never themed. Light mode stays Windows' own light appearance.

The theming is [win32-darkmodelib](https://github.com/ozone10/win32-darkmodelib) commit
`e8ec95ba631462f01e238e63f7d06c1e6829a7a1` (2026-10-10; upstream has no releases or tags).
Its sources are not in git: `runtime/win/fetch-darkmodelib.sh` puts that commit in
`runtime/third_party/darkmodelib` (a nested repository, ignored like the other third-party
folders), then applies `runtime/win/darkmodelib-delayload.patch`. The patch is ours: a
delay-loaded import's slot holds the loader's thunk until the import is first called, and saving
that thunk as the hook's true function made the hook call itself; the patch saves the target DLL's
real export instead. `runtime/win/build-darkmode.sh` calls the fetch and builds `src/*.cpp` as a
static C++ library with the llvm-mingw toolchain, linked into the .exe for ARM64 and x64. Our C
host calls it through `runtime/tools/pvz2-windows-darkmode.cpp`, because the library's functions
are `extern "C"` inside `namespace dmlib` and C cannot name them. The fetched files are changed
only by that patch.

It is MPL 2.0 with some MIT-covered files; the license texts arrive with the fetch
(`LICENSE.md`, `LICENSE-MIT.md`). To update it, change `PIN` in `fetch-darkmodelib.sh`, delete
`runtime/third_party/darkmodelib`, and build again; redo the patch against the new commit.

## Folders

| What | Normal | Portable (`portable` beside the .exe) |
|---|---|---|
| Settings file | `%APPDATA%\PvZ2 Reflourished\settings.ini` | `<exe>\data\settings.ini` |
| The game's state: config, reminders, Nimble | `%APPDATA%\PvZ2 Reflourished` (`config\pvz2-config`, `pvz2-reminders.tsv`, `nimble-persistence.tsv`) | `<exe>\data` |
| The game's saves (`/sdcard/Android/data/<pkg>`) | `%APPDATA%\PvZ2 Reflourished\sdcard\Android\data\<pkg>` | `<exe>\data\sdcard\Android\data\<pkg>` |
| Game data: the OBB, the CDN archive, the x64 translation cache, `pvz2.log`, `cache\` | `%LOCALAPPDATA%\PvZ2 Reflourished` | `<exe>\data` |

`TL_DATA` overrides all of it for tests: everything (settings, saves, game data) goes in that
one folder, so a scripted run touches nothing else. The log, `pvz2.log`, is in the game data
folder and every launch names the three folders in it.

## Settings

`settings.ini` is a plain INI file:

```ini
[keys]
seed1=1
...
seed8=R
shovel=S
fastForward=Tab
plantFood=F
collectSun=Space
powerup1=Z
powerup2=X
powerup3=C
[window]
startFullScreen=0
resizeMode=Fixed
confirmQuit=1
fixedAspect=display
fullScreenState=0
x=...
y=...
width=...
height=...
maximized=0
```

An empty value means "None" for that shortcut. A missing file or a broken value means the
defaults. The Keyboard page of the settings window (Settings... in the window menu or the
taskbar jump list, or Ctrl+,) records a key by clicking its field and pressing the key;
Escape cancels, Delete clears, and a key another shortcut already uses is refused with that
shortcut's name.

The game's taskbar button (and a pinned or Start menu shortcut) has a jump list with
Settings..., plus Check for Updates... in the installed build. A second start, from the jump
list, a double click or a shortcut, hands its request to the running game and quits before it
loads anything. The named mutex is keyed by the data folder's lowercased absolute path, so a
TL_DATA or portable folder is a different game and can run beside the normal one.

The Settings window can be resized by its edges and corners (no maximize button). It never gets
smaller than the size it opens at, at the window's DPI: `WM_GETMINMAXINFO` holds the edges and
`WM_WINDOWPOSCHANGING` holds `SetWindowPos`/`MoveWindow` callers such as AltSnap. The tab area fills
the window, the buttons keep to the bottom right, and sliders, drop-downs, paths and text widen
with it (up to a readable width).

Settings has four tabs: General, Gameplay, Keyboard and About. General selects Fixed
or Adapt mode and, in Fixed, the startup aspect (`display`, `game`, `16:9`, `16:10`,
`3:2`, `4:3`, `21:9`). Fixed constrains interactive resizing to the startup aspect;
Adapt starts in the window's shape, between 4:3 and 2.4:1. Changing mode or aspect
takes effect next start. Current display uses that monitor's shape, without bars
in full screen on supported display shapes. The game's own aspect keeps bars on
a differently shaped monitor. Size, place and full-screen state are always saved.
`keepSizeAndPlace` and `keep_window` are ignored; old `adaptShape=1` (or `adapt=1`)
migrates to Adapt when there is no `resizeMode`. The older keys and pointer
preferences keep their meanings. Pages scroll when the DPI leaves too little room.
Ask before quitting is on by default. Close, Alt+F4 and the system menu's Close
share a Quit/Cancel prompt; only Quit runs the existing background-and-save path.

`runtime/win/build-win.sh settings-test` builds `out/win/rfl-settings-test.exe`,
a native Windows test of defaults, old INI migration and round-trip persistence.
It uses and deletes its own temporary INI, never the player's settings.

The shortcuts are the Mac host's, from `runtime/tools/pvz2-keys.h`: 1-4 and Q W E R pick seed
packets 1-8, S the shovel, F plant food, Tab fast-forward, Space collects suns and coins, and
Z X C the level's power-ups. They use the game's own card press and shovel, as on the Mac.
Gameplay offers the in-level key labels, pointer preview and cross brightness. The pointer
preview ticks at the display's refresh rate with a high-resolution timer, and the plant follows
mouse moves between ticks.

## Updates

The installed build updates itself with [WinSparkle](https://winsparkle.org) 0.9.4. Once a day
it reads the Windows appcast (`sparkle/windows-appcast.xml` on the default branch); "Check for
Updates..." in the window menu and the taskbar jump list, and "Check Now" on Settings' About
page, check at once. The
About page's Updates section (absent from the portable build) has "Check for updates
automatically", on by default (`[updates] automaticChecks` in `settings.ini`), the last check
time, and the check button. Every setup program the feed offers is signed with the project's
EdDSA key, the key that also signs the Mac's DMG; an update is installed only when its
signature verifies. Before the setup runs, WinSparkle calls the host back: the game hears
Android's background event, saves, and exits, and the silent setup starts the new version
again. The feed, the key and the release side: docs/windows-shipping.md.

The portable build carries no updater and no `WinSparkle.dll`; its "Check for Updates..."
opens the releases page in the browser instead. `TL_PVZ2_OPEN_URL=log` makes the host log
that link without opening it, as on the Mac, for tests.

## The game's data (the OBB)

A Windows zip carries no OBB (the game's 1.3 GB of assets), so the first start downloads it
before the game runs, as the Mac and iPad slim apps do: from the link in
`runtime/win/pvz2-obb-win.h` (the first `obb_urls` entry in `data/game-1.4.2.json`, which the
Mac build writes into its Info.plist), checked against the same size and sha256, and put in
`<data>/sdcard/Android/obb/com.ea.game.pvz2_rfl/` through a `.part` file and an atomic move.
A small window shows the progress and a Cancel button; a failure explains itself and offers
Try Again, which resumes a stopped download with a Range request. The window needs about
256 MB more than the OBB free; an old copy is removed first when space is short. The checks
and messages are the Mac client's.

## The key labels in levels

With "Show keys in levels" on (Settings, Gameplay; the default), the host draws the key
that works each control on the control: the seed packets, the shovel, fast-forward, plant
food and the level's power-ups. The driver publishes where the controls are and whether
the board is live (`tl_pvz2_badges`, the same snapshot the Mac and iPad read); the host
draws the key on a click-through layered window over the picture in the Mac's style: yellow
with a thick dark outline and no plaque, so the game shows between and around the letters.
It hides unless the board is live. The label comes from the same settings the keyboard
reads, so a remapped key changes at once; `level-ui.ttf` (beside the program) draws the
letters and digits. It has no Tab or arrow symbols, and the system symbol fonts are too thin
to survive the outline, so the host draws those five as filled paths (a fat arrow, and Tab's
arrow into a bar), sheared like the Mac's italic. Without the file the labels use Segoe UI.

## Live content

`husk-tl-cdn-win.c` fetches the live content over WinHTTP from the project's own CDN
(`https://reflourished.ducnguyen.org/cdn/1.4.2.main.tar`), with the Mac client's rules:
only that host on https port 443, no redirects, 30 s without data and 60 s in total, the
server's `Date` required, and the body must be a complete gzip archive before it replaces
the saved copy atomically. A failed fetch answers with the saved archive dated now.

## The first x64 start

The x86-64 build translates the game before its first frame. The host shows a small window
("Preparing the game for this PC. This happens once.") with a marquee bar while
`tl_x64_translate` runs, and closes it when the first frame is drawn. A start with the
translation already in `<data>/translated` does not show it. ScarletNX has no progress
callback, so the bar is a marquee, not a percentage.

For a bounded UI test without translation, set `TL_PVZ2_PREVIEW_PREPARE=60`
in the x64 build's environment. It shows the same preparing window for 60
seconds, skips the game thread, then exits (maximum 120 seconds).

## The installer

`out/win-installer/PvZ2Reflourished-<version>-windows-setup.exe` is the setup program: one
file for both architectures. On ARM64 Windows it installs the ARM64 program, everywhere else
the x64 one. It is made with Inno Setup 7 on a Windows PC: `runtime/win/installer/build-installer.sh`
stages both trees and (with `--zip`) writes the file to carry them there, and
`runtime/win/installer/build-installer.cmd` runs `pvz2.iss` on it. It installs for the current
user without an administrator prompt into `%LOCALAPPDATA%\Programs\PvZ2 Reflourished`, makes the
Start menu entry (a desktop shortcut is a task that starts unchecked), and Windows' Installed
apps lists it with the game's icon. Neither staged tree carries a `portable` file.

What the download costs: the 1.3.0 setup is 120 MB. Its ARM64 tree is 65 MB and its x64 tree
229 MB, which share the APK and ANGLE. A separate ARM64 setup would be about 46 MB and a
separate x64 one about 74 MB, so one setup gives an ARM64 player 120 MB instead of 46 and an
x64 player 120 instead of 74. The project keeps one setup: one download link, one feed item
shape, and one file to sign.

Uninstalling deletes the program folder and `%LOCALAPPDATA%\PvZ2 Reflourished` (the OBB, the
CDN archive, the x64 translation cache and the log), then asks "Also delete your saves and
settings?" with No selected, about `%APPDATA%\PvZ2 Reflourished`; a silent uninstall keeps them.
An install over an older version (the same AppId) keeps both data folders. If the game is
running, Setup offers to close it, and a close from another program skips the game's own
"Ask before quitting" prompt (that prompt is for the player at the window) and still saves.

## Zips

`runtime/win/package-zip.sh --arch all --apk <apk> [--tar <1.4.2.main.tar>]` makes
`out/zips/PvZ2Reflourished-<version>-windows-{arm64,x64}.zip`: the portable program, ANGLE and
its license, `Game/` (the APK and the CDN archive), a `portable` file and a README. The
`portable` file is what makes the program keep everything in `data\` beside the .exe (saves,
settings, the OBB, the x64 translation cache and the log), so deleting the folder removes all
of it, and the zip has no updater: its "Check for Updates..." opens the releases page. The
script takes the portable programs from `out/win-portable` and `out/win-x64-portable`
(`build-win.sh game-portable`, `build-x64.sh game-portable`), takes ANGLE from
`prebuilt/win-arm64` and `prebuilt/win-x64`, which `fetch-angle.sh` fills first (or pass
`--angle`), and the x64 program's compiler DLLs from `out/win/x64-runtime` (`--x64-runtime`).

The ARM64 program is linked statically and imports only Windows DLLs. The x64 program
imports `libLLVM-23.dll`, `libwinpthread-1.dll`, `libc++.dll`, `libffi-8.dll`, `zlib1.dll`,
`libzstd.dll`, `libxml2-16.dll` and `libiconv-2.dll` from MSYS2's clang64 (LLVM 23); they go
in the zip beside the program, and Windows' own DLLs come from the system. See
`docs/windows-shipping.md` for where the ANGLE copies come from and how to refresh them.

## Build

```sh
export TL_WIN_TOOLCHAIN=~/somewhere/win-toolchain   # an empty folder the first time
runtime/win/setup-toolchain.sh                      # llvm-mingw, and the two zlibs
runtime/win/build-win.sh starttest                  # out/win/rfl-starttest.exe
```

`rfl-starttest.exe <apk> <data dir>` loads the four libraries, runs their constructors and
JNI_OnLoad, and the mod's library, and logs each step.
