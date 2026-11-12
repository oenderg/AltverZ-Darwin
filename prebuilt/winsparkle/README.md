# WinSparkle for Windows

`arm64/WinSparkle.dll` and `x64/WinSparkle.dll`, with the import libraries of the same names:
WinSparkle 0.9.4, the updater the installed Windows build uses (the zip and portable builds
carry no updater and do not link it). `include/` has its API headers.

The files are not in git. `runtime/win/fetch-winsparkle.sh` downloads
`WinSparkle-0.9.4.zip` from its GitHub release, checks GitHub's published sha256 for that asset
(`6037df37fc263bd1650a1c4949681a9d40ffe991d01f35892a406cb5d103c976`) and unpacks the two
architectures here. The release also holds Win32 (x86) binaries and `.pdb` files, which are left
out.

WinSparkle is MIT licensed: see `COPYING`; its copy of expat is under `COPYING.expat`.
