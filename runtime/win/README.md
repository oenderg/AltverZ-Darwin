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

`runtime/win/build-win.sh game` builds it from the shared driver, the Windows host
(`runtime/tools/pvz2-windows.c`) and the settings window
(`runtime/tools/pvz2-windows-settings.c`). It needs `libEGL.dll` and `libGLESv2.dll` (ANGLE)
beside it, and `Game/pvz2.apk` (with `Game/1.4.2.main.tar` when the APK is the slim one).

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
at once and uploads one artifact per architecture. The DLLs committed in `prebuilt/win-*` come
from it, and the READMEs there name the run. docs/windows-shipping.md has the pinned commit,
the build arguments and how to refresh them.

## Folders

| What | Normal | Portable (`portable` beside the .exe) |
|---|---|---|
| Settings file | `%APPDATA%\PvZ2 Reflourished\settings.ini` | `<exe>\data\settings.ini` |
| The game's saves (`/sdcard/Android/data/<pkg>`) | `%APPDATA%\PvZ2 Reflourished\sdcard\Android\data\<pkg>` | `<exe>\data\sdcard\Android\data\<pkg>` |
| Game data: the OBB, the CDN archive, the x64 translation cache | `%LOCALAPPDATA%\PvZ2 Reflourished` | `<exe>\data` |

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
keepSizeAndPlace=1
x=...
y=...
width=...
height=...
maximized=0
```

An empty value means "None" for that shortcut. A missing file or a broken value means the
defaults. The Keyboard page of the settings window (window menu: Settings..., or Ctrl+,)
records a key by clicking its field and pressing the key; Escape cancels, Delete clears,
and a key another shortcut already uses is refused with that shortcut's name.

The shortcuts are the Mac host's, from `runtime/tools/pvz2-keys.h`: 1-4 and Q W E R pick seed
packets 1-8, S the shovel, F plant food, Tab fast-forward, Space collects suns and coins, and
Z X C the level's power-ups. They use the game's own card press and shovel, as on the Mac.
There are no in-level key labels yet on Windows, so the Mac's "Show keys in levels" choice is
not offered.

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

With "Show keys in levels" on (Settings, Keyboard; the default), the host draws the key
that works each control on the control: the seed packets, the shovel, fast-forward, plant
food and the level's power-ups. The driver publishes where the controls are and whether
the board is live (`tl_pvz2_badges`, the same snapshot the Mac and iPad read); the host
draws a small dark plaque with the yellow key on a click-through layered window over the
picture, and hides it unless the board is live. The label comes from the same settings the
keyboard reads, so a remapped key changes at once, and `level-ui.ttf` (beside the program)
draws it; without the file a bold italic system face is used.

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

## Zips

`runtime/win/package-zip.sh --arch all --apk <apk> [--tar <1.4.2.main.tar>]` makes
`out/zips/PvZ2Reflourished-<version>-windows-{arm64,x64}.zip`: the program, ANGLE and its
license, `Game/` (the APK and the CDN archive) and a README. It takes ANGLE from
`prebuilt/win-arm64` and `prebuilt/win-x64` (or `--angle`), and the x64 program's compiler
DLLs from `out/win/x64-runtime` (`--x64-runtime`).

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
