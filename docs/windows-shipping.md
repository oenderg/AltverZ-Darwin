# Shipping the Windows builds

Two zips come out of `runtime/win/package-zip.sh`:

```
PvZ2Reflourished-<version>-windows-arm64.zip
PvZ2Reflourished-<version>-windows-x64.zip
```

Each holds the program, ANGLE (`libEGL.dll`, `libGLESv2.dll` and the D3D/Vulkan
companions), `Game/pvz2.apk`, `Game/1.4.2.main.tar`, the level key label font
(`level-ui.ttf`) and a README. No MSYS2 or
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

The current copies:

- ARM64: `libEGL.dll` and `libGLESv2.dll`, version 2.1.41232 (git 08d326a25514),
  Microsoft-signed, from the Edge ANGLE the ARM64 port was proved with. The exact Edge
  folder they were taken from was not recorded; they were staged in the test machine's
  `C:\rfl\game` folder. `d3dcompiler_47.dll` (Microsoft), `vk_swiftshader.dll` and
  `vulkan-1.dll` travel with them for the D3D11 and Vulkan/SwiftShader paths.
- x86-64: `libEGL.dll` and `libGLESv2.dll`, version 2.1.25748 (git 890b5d8fa298), from
  MSYS2's `clang64` (the package `mingw-w64-clang-x86_64-angleproject`). The x86-64 port
  was proved with these.

Nothing was built or downloaded for ANGLE tonight. The options for a copy the project
controls are:

1. Build ANGLE from source for both architectures. The repository already builds it for
   iOS and the Simulator (`runtime/scripts/build_angle_ios.sh`); a Windows script would
   run ANGLE's GN/ninja build with the D3D11 backend and copy the two DLLs (and
   `d3dcompiler_47.dll`) into `out/win/angle-arm64` and `out/win/angle-x64`. This is the
   only option that pins a version and works on ARM64 and x86-64 with the same source.
2. Ship Microsoft Edge's ANGLE DLLs, as the ARM64 test machine has. They are signed and
   fast, but their version follows Edge and their redistribution terms have to be checked.
3. Use MSYS2's ANGLE packages for both architectures (x86-64 is what the x64 build uses;
   the `mingw-w64-clang-aarch64-angleproject` package would cover ARM64). That ties the
   player's graphics to MSYS2's package, which is why it is a test option, not the plan.

Until one of those is chosen, `package-zip.sh` takes the DLLs from `out/win/angle-arm64`
and `out/win/angle-x64`; the zips are not published from this branch.
