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

## Where the player's files go

`%APPDATA%\PvZ2 Reflourished` is the small, personal folder:

- `settings.ini`;
- `config\pvz2-config`, the game's own options and tutorial state;
- `pvz2-reminders.tsv` and `nimble-persistence.tsv`;
- the saves, under `sdcard\Android\data\com.ea.game.pvz2_rfl\files\...`, including
  `No_Backup\CDN.1.4.2`.

`%LOCALAPPDATA%\PvZ2 Reflourished` is the big folder that can be made again on this PC:

- the OBB under `sdcard\Android\obb\...`;
- `No_Backup\1.4.2.main.tar`;
- the x64 translation cache and `cache\`;
- `pvz2.log`.

Portable mode and `TL_DATA` keep everything in one folder, as before. A start with the
old layout moves `config\pvz2-config` (or its newer `.tmp`), the reminders and Nimble
persistence from the big folder to the small one once, and logs each move.

## What each program imports

`llvm-objdump -p` on the ARM64 program (`out/win/PvZ2 Reflourished.exe`, linked with
`-static` by llvm-mingw):

```
WINMM.dll SHELL32.dll COMCTL32.dll WINHTTP.dll bcrypt.dll GDI32.dll GDIPLUS.dll USER32.dll KERNEL32.dll
api-ms-win-core-synch-l1-2-0.dll
api-ms-win-crt-{stdio,filesystem,runtime,time,math,heap,environment,string,locale,convert,utility,private}-l1-1-0.dll
```

All of those are Windows components (the API sets are the Universal CRT). The program
has no non-system dependency.

`llvm-objdump -p` on the x86-64 program (`out/win-x64/PvZ2 Reflourished x64.exe`):

```
WINMM.dll SHELL32.dll COMCTL32.dll WINHTTP.dll bcrypt.dll GDI32.dll GDIPLUS.dll USER32.dll KERNEL32.dll ADVAPI32.dll
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

## The installer

`runtime/win/installer/build-installer.sh --apk <apk> [--tar <tar>] --zip` stages the two
architecture trees in `out/win-installer/` and zips them for a Windows PC;
`runtime/win/installer/build-installer.cmd <staged folder> [version] [output folder]` compiles
`pvz2.iss` with Inno Setup 7 (7.1.1 while this was written; it also runs on ARM64 Windows,
through emulation) into `PvZ2Reflourished-<version>-windows-setup.exe`. Both are per user, with
no administrator prompt, and install to `%LOCALAPPDATA%\Programs\PvZ2 Reflourished`; a fixed
AppId ties the versions together.

The 1.3.0 setup is 120 MB. The ARM64 files are 65 MB and the x64 files 229 MB, with the game's
APK and ANGLE in both; a setup for one architecture alone would be about 46 MB (ARM64) or 74 MB
(x64). One setup is kept because it is one download link and one feed item, at the cost of the
larger download for each player.

Uninstalling deletes the program folder and `%LOCALAPPDATA%\PvZ2 Reflourished` (the OBB, the CDN
archive, the x64 translation cache and `pvz2.log`), then asks "Also delete your saves and
settings?" with No preselected; Yes deletes `%APPDATA%\PvZ2 Reflourished` (saves, settings,
`config\pvz2-config`, the reminders and Nimble files). A silent uninstall keeps them. An install
over an older version keeps both folders; Setup offers to close a running game, and a close that
comes from Setup (or another program) saves and exits without the game's own quit prompt.

## Updates (WinSparkle) and the Windows appcast

The installed build updates itself with WinSparkle 0.9.4 (MIT; `prebuilt/winsparkle`, fetched by
`runtime/win/fetch-winsparkle.sh`, which checks GitHub's published sha256). The zip build does
not link it.

### The feed: `sparkle/windows-appcast.xml`

A separate file on the default branch from the Mac's `sparkle/appcast.xml`. The Mac feed is
written from a DMG by `scripts/update-appcast.py` and carries Mac-only items; the Windows feed
has the setup program, two `sparkle:os` enclosures and no Mac fields. Two files mean neither
platform's release can rewrite the other's feed, and a Windows release can be held back without
touching the Mac app's updates. The app's feed URL is compiled in
(`runtime/win/build-win.sh`); `SPARKLE_FEED_URL` overrides it for a test build, as
`SPARKLE_PUBLIC_KEY` does for the key.

One item, with one enclosure per architecture: both point at the same setup program (the one
installer carries both builds), with `sparkle:version` being ours as a build number
(`major*10000 + minor*100 + patch`, so 1.3.0 is 10300) and `sparkle:shortVersionString` the
version. WinSparkle picks the enclosure whose `sparkle:os` matches the running Windows, so an
ARM64 player and an x64 player each get the setup that installs their build. (Two items with
one enclosure each work the same way; the `sparkle:os` marker lives on the enclosure either
way.)

```xml
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>PvZ2 Reflourished for Windows</title>
    <link>https://github.com/oenderg/Reflourished-Darwin/releases</link>
    <description>Updates of PvZ2 Reflourished for Windows</description>
    <language>en</language>
    <item>
      <title>Reflourished-Darwin 1.3.1 for Windows</title>
      <pubDate>...</pubDate>
      <link>https://github.com/oenderg/Reflourished-Darwin/releases/tag/v1.3.1</link>
      <sparkle:version>10301</sparkle:version>
      <sparkle:shortVersionString>1.3.1</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>10.0</sparkle:minimumSystemVersion>
      <description><![CDATA[<p>What's new...</p>]]></description>
      <enclosure url="https://github.com/oenderg/Reflourished-Darwin/releases/download/v1.3.1/PvZ2Reflourished-1.3.1-windows-setup.exe"
                 length="120182790" type="application/octet-stream"
                 sparkle:edSignature="..."
                 sparkle:os="windows-x64"
                 sparkle:installerArguments="/SILENT /SP- /NOICONS"/>
      <enclosure url="https://github.com/oenderg/Reflourished-Darwin/releases/download/v1.3.1/PvZ2Reflourished-1.3.1-windows-setup.exe"
                 length="120182790" type="application/octet-stream"
                 sparkle:edSignature="..."
                 sparkle:os="windows-arm64"
                 sparkle:installerArguments="/SILENT /SP- /NOICONS"/>
    </item>
  </channel>
</rss>
```

`sparkle:os` and `sparkle:installerArguments` are **enclosure** attributes in WinSparkle's
parser, not item attributes (its `src/appcast.cpp` reads them there). WinSparkle picks the
architecture-specific enclosure of the newest compatible item; `sparkle:minimumSystemVersion`
is a Windows version, so 10.0 leaves out Windows 7 and 8.

### The key: the Mac app's EdDSA key

One private key signs both platforms. Sparkle and WinSparkle both use Ed25519 over the file's
bytes, and the same 32-byte public key file (`sparkle/public-key`) is compiled into the Windows
program, so a second key would only add a second secret to create, back up and lose. The Mac
key is in the owner's login keychain and in the Actions secret `SPARKLE_PRIVATE_KEY`
(docs/RELEASING.md).

Signing on a Mac, with the Sparkle build the Mac app already fetches:

```sh
sig="$(<sparkle>/bin/sign_update --ed-key-file <key file> PvZ2Reflourished-1.3.1-windows-setup.exe)"
```

Signing on Windows, with `winsparkle-tool` from the release (`bin\winsparkle-tool.exe sign
--private-key-file key.pem <setup.exe>`). It wants a PEM private key; Sparkle's key file is the
base64 of the 32-byte seed, which becomes a PKCS#8 Ed25519 key with the prefix `302e0201003005
06032b657004220420` and PEM armour. Never put either form in the repository or a build log.

### Release steps

The Windows builds are made on the release Mac and in a Windows PC or VM (the x86-64 program
needs MSYS2), not in GitHub Actions, so a Windows release is three steps after the Mac and iOS
release exists (docs/RELEASING.md, "Windows updates"):

1. Build the setup with `runtime/win/installer/build-installer.sh` and `build-installer.cmd`
   from the release's APK, with `VERSION` as its version.
2. On the Mac: `scripts/publish-windows-setup.sh PvZ2Reflourished-<version>-windows-setup.exe
   --tag <tag> --upload --commit`. It signs the setup with the Mac app's key from the login
   keychain (Sparkle's `sign_update`; `--key-file` takes the exported key instead), checks the
   signature against `sparkle/public-key`, the key the Windows program carries, attaches the
   setup to the release, writes `sparkle/windows-appcast.xml` with
   `scripts/update-windows-appcast.py` (the release notes from CHANGELOG, as the Mac feed has
   them), validates it and commits it (`chore(sparkle-windows): ...`). Without `--upload` and
   `--commit` it only signs, checks and writes the feed.
3. Push the commit to the default branch: that is when installed copies see the update.

The feed offers only the newest release, as the Mac feed does: the setup is a whole install,
so a player on any older version goes straight to the newest one. The release workflow and the
Mac feed are not changed by any of this.

The app checks once a day at start-up (Settings > About: "Check for updates automatically", on
by default, `[updates] automaticChecks` in `settings.ini`) and at once from "Check for Updates..."
or "Check Now". When an update is accepted, WinSparkle runs the setup with
`/SILENT /SP- /NOICONS`; the game hears the shutdown callback, saves and exits, and the setup's
silent-update `[Run]` entry starts the new version again. The setup keeps both data folders, so
the saves are there in the new version. A signature that does not verify refuses the update, as
does a tampered file.

### Trying it without a release

A test build needs its own feed and a throwaway key: build with
`SPARKLE_FEED_URL=http://127.0.0.1:8799/windows-appcast.xml SPARKLE_PUBLIC_KEY=<test>`, serve
the feed and the setup from the Mac (the VM reaches `127.0.0.1` as `http://10.0.2.2:<port>/`),
and write the appcast with the test key's signature. The VM's WinSparkle state lives in
`HKCU\Software\oenderg\PvZ2 Reflourished\WinSparkle`; delete it after a test, and never sign a
test with the real key.

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
