# Shipping the Windows builds

Two zips come out of `runtime/win/package-zip.sh`:

```
PvZ2Reflourished-<version>-windows-arm64.zip
PvZ2Reflourished-<version>-windows-x64.zip
```

Each holds the program, ANGLE (`libEGL.dll`, `libGLESv2.dll` and `d3dcompiler_47.dll`),
`Game/pvz2.apk`, `Game/1.4.2.main.tar`, the level key label font
(`level-ui.ttf`) and a README. Neither zip carries the game's OBB (1.3 GB): the
first start downloads it from the Mac build's own link (`data/game-1.4.2.json`'s
first `obb_urls` entry) into the data folder, checking its size and sha256, with a
progress window and a Try Again after a failure. No MSYS2 or
llvm-mingw installation is needed on the player's PC: everything the programs import
beyond Windows' own DLLs travels in the zip.

## What each program imports

`llvm-objdump -p` on the ARM64 program (`out/win/PvZ2 Reflourished.exe`, linked with
`-static` by llvm-mingw):

```
WINMM.dll SHELL32.dll COMCTL32.dll WINHTTP.dll bcrypt.dll GDI32.dll USER32.dll KERNEL32.dll
api-ms-win-core-synch-l1-2-0.dll
api-ms-win-crt-{stdio,filesystem,runtime,time,math,heap,environment,string,locale,convert,utility,private}-l1-1-0.dll
```

All of those are Windows components (the API sets are the Universal CRT). The program
has no non-system dependency.

`llvm-objdump -p` on the x86-64 program (`out/win-x64/PvZ2 Reflourished x64.exe`):

```
WINMM.dll SHELL32.dll COMCTL32.dll WINHTTP.dll bcrypt.dll GDI32.dll USER32.dll KERNEL32.dll ADVAPI32.dll
api-ms-win-core-synch-l1-2-0.dll, api-ms-win-crt-* (as above)
libwinpthread-1.dll, libLLVM-23.dll
```

`libLLVM-23.dll` imports `libffi-8.dll`, `zlib1.dll`, `libzstd.dll`, `libxml2-16.dll`,
`libc++.dll` and `libiconv-2.dll`; those are the other files the zip adds. They all come
from MSYS2's `clang64` tree (LLVM 23), the toolchain the x86-64 build is made with. The
zip needs all eight:

```
libLLVM-23.dll libwinpthread-1.dll libc++.dll libffi-8.dll zlib1.dll libzstd.dll
libxml2-16.dll libiconv-2.dll
```

A static LLVM link would remove `libLLVM-23.dll` and its companions, but MSYS2's static
LLVM is 440 MB of component libraries and the link is a separate piece of work; the
dynamic library is what the build uses now.

## ANGLE

Both programs draw through ANGLE over Direct3D 11, and the zips carry our own build of it:
`libEGL.dll` and `libGLESv2.dll` from one ANGLE commit for x64 and ARM64, and
`d3dcompiler_47.dll` for the same architecture (the Windows SDK's copy, which ANGLE's D3D11
backend loads to compile shaders). ANGLE's license travels as `LICENSE-ANGLE.txt`.

The copies this replaces were Edge's ANGLE 2.1.41232 (git 08d326a25514, its source folder not
recorded) with `vk_swiftshader.dll` and `vulkan-1.dll` for ARM64, and MSYS2's 2.1.25748 (git
890b5d8fa298), MinGW built, for x86-64. Neither could be rebuilt from a pinned commit and the
two architectures did not share a version: Edge's copy follows Edge's releases and its
redistribution terms were never checked, and MSYS2's ties the player's graphics to that
distribution. Of the three routes this document used to list, the project builds its own
ANGLE: the only one that pins a version and covers both architectures from one source.

### How the build works

`runtime/win/build-angle.cmd [x64|arm64|all]` builds it, and
`.github/workflows/angle-windows.yml` runs that script on `windows-2025`, once per
architecture, in parallel. It does what Chrome's own ANGLE build does:

1. `depot_tools` into `out/win/angle-build/depot_tools` (the workflow caches it), then ANGLE
   at `runtime/win/ANGLE_COMMIT`, with a shallow fetch of that one commit.
2. `scripts/bootstrap.py`, which writes `.gclient`.
3. `runtime/win/trim-angle-deps.py` drops the DEPS entries a D3D11 renderer does not need:
   catapult, dawn, `third_party/llvm/src`, SwiftShader and `VK-GL-CTS/src`. They are large
   checkouts, and nothing in a D3D11 build reads them.
4. `gclient sync -f -D -R`: Chromium's build files, Chromium's clang and libc++, gn, ninja
   and the smaller third party libraries.
5. `gn gen out/<arch>`, whose arguments are:

   ```
   target_cpu="x64"          (or "arm64")
   angle_build_all=false
   is_debug=false
   angle_has_frame_capture=false
   angle_enable_gl=false
   angle_enable_vulkan=false
   angle_enable_wgpu=false
   angle_enable_null=false
   use_siso=false
   ```

6. `autoninja -C out/<arch> libEGL libGLESv2`.
7. The DLLs, `d3dcompiler_47.dll`, `LICENSE`, `commit.txt` and `gn-args.txt` into
   `prebuilt/win-<arch>`, then a check of the DLLs' imports with `dumpbin /dependents`.

Chrome's compiler (Chromium's clang, which `is_clang` selects for Windows), a static C runtime
(`/MT`, which Chromium's non component Windows builds use), `is_debug=false` and DCHECKs off:
a player needs no Visual C++ redistributable and the DLLs carry no debug assertions.

cmd and not PowerShell because the script has to run from any Windows prompt as it is, and a
`.ps1` is refused by the default execution policy on many PCs. The one step cmd cannot express,
a DEPS removal that counts braces instead of lines, is a small Python file run under the Python
depot_tools brings (`vpython3`). On a PC with Visual Studio (C++ for x64 and ARM64) and the SDK
the script names, the same script builds it; `set ANGLE_COMMIT=<sha>` builds another commit.

### What it imports

`llvm-objdump -p` on `libGLESv2.dll` from mmozeiko/build-angle's 2026-11-08 zips, which are the
same configuration (D3D11 only, static C runtime), x64 and ARM64 alike:

```
KERNEL32.dll USER32.dll api-ms-win-core-synch-l1-2-0.dll dxgi.dll
```

No `vulkan-1.dll` and no `vk_swiftshader.dll`: the D3D11 backend is compiled in and loads
`d3d11.dll` and `d3dcompiler_47.dll` at run time, so the zips no longer carry those two.
`libEGL.dll` imports `KERNEL32.dll` alone. `build-angle.cmd` runs the same check on its own
output and stops if a DLL would want the Visual C++ runtime, Vulkan or SwiftShader.

### The pinned commit

`runtime/win/ANGLE_COMMIT` holds it. It is the ANGLE revision of the Chromium DEPS of the
current Chrome stable release, here `a9d273575638975f4f31aff9523c5194f36ecb61`
(Chrome 158.0.8127.18):

```
$ curl -s 'https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Windows&num=1'
  "hashes": { "angle": "a9d273575638975f4f31aff9523c5194f36ecb61",
              "chromium": "6449d9f9a6a906edff724b7b220199b3fde4615c", ... },
  "version": "158.0.8127.18"
$ curl -s 'https://chromium.googlesource.com/chromium/src/+/6449d9f9.../DEPS?format=TEXT' \
    | base64 -d | grep angle_revision
  'angle_revision': 'a9d273575638975f4f31aff9523c5194f36ecb61',
```

The two agree, and that agreement is the check: chromiumdash reads the same DEPS. ANGLE keeps
the branch a release came from as `refs/heads/chromium/<build number>` (`git ls-remote` on
`https://chromium.googlesource.com/angle/angle` lists them); the commit must be in that
branch's history, which it is here (`chromium/8127`).

### Refreshing ANGLE

1. Find the new commit as above and put it in `runtime/win/ANGLE_COMMIT`, the one place the
   pin lives. The workflow's input has an empty default and reads that file.
2. Run `.github/workflows/angle-windows.yml`: push to `ci/angle-windows`, or Actions, "ANGLE
   for Windows", "Run workflow" with the commit left empty.
3. Zip each artifact's files (the three DLLs, `commit.txt`, `gn-args.txt`, `run.txt`,
   `LICENSE`) as `angle-windows-<arch>-<first 8 of the commit>.zip` and upload both to the
   repository's private draft release `build-inputs`, next to the APK. Keep the old zips there
   so older commits still build.
4. Put the two names and their sha256 in `runtime/win/ANGLE_ZIPS` and update the commit line of
   `prebuilt/win-x64/README.md` and `prebuilt/win-arm64/README.md`.
5. Run `runtime/win/package-zip.sh`. It runs `runtime/win/fetch-angle.sh` first, which downloads
   the zips `ANGLE_ZIPS` names (with `gh`, signed in to an account that can see the draft),
   checks their sha256 and unpacks them into `prebuilt/win-<arch>`. `--angle` still overrides
   that for a test build, for example with DLLs `build-angle.cmd` made on a PC.

The DLLs are not in git: every refresh would add about 24 MB to the repository's history for
good. The player zips carry them, so the game needs no download for ANGLE.

The runner is x64, so the ARM64 build is a cross build with the image's ARM64 MSVC tools and
libraries, which is how Chrome's ANGLE builds and mmozeiko/build-angle build it on the same
image. A run takes about 20 to 30 minutes per architecture. The workflow installs the Windows
SDK the commit's build names (10.0.28000.0 for this one) when the image lacks it, about five
minutes on top.
