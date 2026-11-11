PvZ2: Reflourished for Windows: licenses
=========================================

The program itself (PvZ2 Reflourished.exe) is free software under the GNU General
Public License, version 2 or (at your option) any later version: PvZ2-Reflourished.txt
here. Its source code is at https://github.com/oenderg/Reflourished-Darwin, the
release's tag names the exact version.

It is built with these other works, each under its own license:

  darkmodelib (win32-darkmodelib by ozone10), built into the program
      darkmodelib-MPL-2.0.txt, darkmodelib-MIT.txt
      source: https://github.com/ozone10/win32-darkmodelib, the commit that
      runtime/win/fetch-darkmodelib.sh names, with runtime/win/darkmodelib-delayload.patch
  Zstandard's decoder (Meta Platforms), built into the program
      zstd.txt
  ANGLE (libEGL.dll, libGLESv2.dll), beside the program
      LICENSE-ANGLE.txt, one folder up
  WinSparkle (WinSparkle.dll), in the installed copy only
      LICENSE-WinSparkle.txt, one folder up

The Intel/AMD (x64) program also contains:

  ScarletNX's recompiler library, GNU General Public License version 3
      ScarletNX-GPL-3.0.txt
      source: https://github.com/scarletnx/ScarletNX at commit
      7950099a3377c3f95ef62c7559dccca4a4911885, with
      runtime/win/x64/scarletnx/scarletnx.patch from this program's source applied.
      Because of it, the x64 program as a whole is distributed under the GNU General
      Public License version 3, which the "or any later version" above allows.

and ships these libraries beside it, from MSYS2's CLANG64 packages
(https://packages.msys2.org, source packages there):

  libLLVM-23.dll        LLVM 23, Apache 2.0 with LLVM exceptions    LLVM.txt
  libc++.dll            LLVM libc++, the same                       libc++.txt
  libffi-8.dll          libffi, MIT                                 libffi.txt
  libiconv-2.dll        GNU libiconv, LGPL 2.1 or later             libiconv.txt
  libxml2-16.dll        libxml2, MIT                                libxml2.txt
  zlib1.dll             zlib, zlib license                          zlib.txt
  libzstd.dll           Zstandard, BSD                              zstd.txt
  libwinpthread-1.dll   mingw-w64 winpthreads, MIT and others       winpthreads.txt

The LGPL libraries are separate DLLs: you may replace them with your own builds.

