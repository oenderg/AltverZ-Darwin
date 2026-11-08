# ANGLE for Windows ARM64

`libEGL.dll`, `libGLESv2.dll` and `d3dcompiler_47.dll`: our build of ANGLE over Direct3D 11
for ARM64, the copy that `PvZ2Reflourished-<version>-windows-arm64.zip` carries.
The files are not in git: `runtime/win/fetch-angle.sh` (which `runtime/win/package-zip.sh` runs)
downloads them from the private draft release `build-inputs`, checks the sha256 that
`runtime/win/ANGLE_ZIPS` names and unpacks them here. `runtime/win/build-angle.cmd` writes here too.

- Commit: `a9d273575638975f4f31aff9523c5194f36ecb61`, the ANGLE that Chrome 158.0.8127.18 builds.
- The run that made them: the `angle-windows-arm64` artifact of `.github/workflows/angle-windows.yml`,
  run 42459025816, whose URL is in `run.txt` beside them.
- Refreshing them: docs/windows-shipping.md, "Refreshing ANGLE".

`commit.txt` names the commit alone, `gn-args.txt` the exact `gn` arguments, `run.txt` the
workflow run, and `LICENSE` is ANGLE's license. Keep all four with the DLLs.
