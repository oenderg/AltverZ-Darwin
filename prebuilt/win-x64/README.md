# ANGLE for Windows x86-64

`libEGL.dll`, `libGLESv2.dll` and `d3dcompiler_47.dll`: our build of ANGLE over Direct3D 11
for x86-64, the copy that `PvZ2Reflourished-<version>-windows-x64.zip` carries.
`runtime/win/package-zip.sh` takes the DLLs from this folder, and `runtime/win/build-angle.cmd`
writes them here.

- Commit: `a9d273575638975f4f31aff9523c5194f36ecb61`, the ANGLE that Chrome 158.0.8127.18 builds.
- The run that made them: the `angle-windows-x64` artifact of `.github/workflows/angle-windows.yml`,
  whose URL is in `run.txt` beside them. Without the DLLs here the zips cannot be made yet.
- Refreshing them: docs/windows-shipping.md, "Refreshing ANGLE".

`commit.txt` names the commit alone, `gn-args.txt` the exact `gn` arguments, `run.txt` the
workflow run, and `LICENSE` is ANGLE's license. Keep all four with the DLLs.
