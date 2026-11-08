@echo off
setlocal enabledelayedexpansion

rem Builds the ANGLE the Windows zips ship: EGL and GLES over Direct3D 11, for x64 and
rem ARM64, from one commit. It leaves libEGL.dll, libGLESv2.dll, d3dcompiler_47.dll,
rem commit.txt, gn-args.txt and LICENSE in prebuilt\win-<arch>, where package-zip.sh
rem takes them from; everything it downloads (depot_tools, the ANGLE checkout) stays in
rem out\win\angle-build.
rem
rem   runtime\win\build-angle.cmd             both architectures
rem   runtime\win\build-angle.cmd x64         one of them, x64 or arm64
rem   set ANGLE_COMMIT=<sha> & runtime\win\build-angle.cmd arm64
rem
rem It needs git, network access, and Visual Studio with the C++ workload for x64 and
rem ARM64 plus the Windows SDK the ANGLE commit's build names (it says which one, and
rem how to add it, when that SDK is missing).
rem
rem cmd and not PowerShell: this runs from any Windows prompt as it is, where a .ps1 is
rem refused by the default execution policy on many PCs. The DEPS edit is the one part
rem cmd cannot do, so it is a small Python helper; depot_tools brings the Python that
rem both it and bootstrap.py run under (vpython3).
rem
rem .github\workflows\angle-windows.yml runs this same script on windows-2025.

set "REPO=%~dp0..\.."
pushd "%REPO%"
set "REPO=%CD%"
popd
set "PINFILE=%REPO%\runtime\win\ANGLE_COMMIT"

rem The architecture. Both is two runs of this script, so each starts from a clean
rem environment; a second gclient sync over an up to date tree is cheap.
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=all"
if /i "%ARCH%"=="all" (
    call "%~f0" x64 || exit /b 1
    call "%~f0" arm64 || exit /b 1
    exit /b 0
)
if /i not "%ARCH%"=="x64" if /i not "%ARCH%"=="arm64" (
    echo Usage: build-angle.cmd [x64^|arm64^|all]
    exit /b 2
)

rem The commit: ANGLE_COMMIT if the caller set it, else the pin in runtime\win\ANGLE_COMMIT
rem (the only line there that starts with a hex digit).
set "PIN=%ANGLE_COMMIT%"
if not defined PIN (
    for /f "delims=" %%c in ('findstr /r /c:"^[0-9a-fA-F][0-9a-fA-F]" "%PINFILE%"') do set "PIN=%%c"
)
if not defined PIN (
    echo No ANGLE commit: put one in runtime\win\ANGLE_COMMIT or set ANGLE_COMMIT
    exit /b 1
)
if "%PIN:~39,1%"=="" goto :bad_commit
if not "%PIN:~40,1%"=="" goto :bad_commit
rem Delayed expansion below: a value from the environment is not re-read as a command.
echo !PIN!| findstr /r /c:"^[0-9a-fA-F][0-9a-fA-F]*$" >nul || goto :bad_commit
goto :commit_ok
:bad_commit
echo Not a full ANGLE commit: !PIN!
exit /b 1
:commit_ok

set "ROOT=%REPO%\out\win\angle-build"
set "ANGLE=%ROOT%\angle"
set "DEPOT=%ROOT%\depot_tools"
set "STAGE=%REPO%\prebuilt\win-%ARCH%"
set "OUT=out\%ARCH%"
if not exist "%ROOT%" mkdir "%ROOT%"

rem depot_tools brings gn, ninja and gclient. DEPOT_TOOLS_WIN_TOOLCHAIN=0 tells gn to
rem use this machine's Visual Studio, not Google's packaged toolchain.
set "PATH=%DEPOT%;%PATH%"
set "DEPOT_TOOLS_WIN_TOOLCHAIN=0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio was not found: install it with the C++ workload for x64 and ARM64
    exit /b 1
)

if not exist "%DEPOT%\gclient.bat" (
    if exist "%DEPOT%" (
        echo %DEPOT% is there but is not depot_tools: delete it and run again
        exit /b 1
    )
    echo ==^> depot_tools
    call git clone --depth=1 --no-tags --single-branch https://chromium.googlesource.com/chromium/tools/depot_tools.git "%DEPOT%" || exit /b 1
)

if not exist "%ANGLE%\.git" (
    if not exist "%ANGLE%" mkdir "%ANGLE%"
    call git -C "%ANGLE%" init -q || exit /b 1
    call git -C "%ANGLE%" remote add origin https://chromium.googlesource.com/angle/angle.git || exit /b 1
)

echo ==^> ANGLE !PIN! for %ARCH%
rem One commit is enough and a third of a gigabyte less than the history. Not every git
rem server allows a shallow fetch of a commit, so fall back to the whole history.
call git -C "%ANGLE%" fetch --depth 1 --no-recurse-submodules origin !PIN! >nul 2>&1
if errorlevel 1 (
    echo    the shallow fetch failed, fetching the whole history instead
    call git -C "%ANGLE%" fetch --no-recurse-submodules origin !PIN! || exit /b 1
)
call git -C "%ANGLE%" reset --hard FETCH_HEAD || exit /b 1

call :prepare || exit /b 1
call :build || exit /b 1
call :stage || exit /b 1
call :imports
if errorlevel 1 exit /b 1

echo ==^> %STAGE%
dir /b "%STAGE%"
exit /b 0

rem ---------------------------------------------------------------------------------

:prepare
pushd "%ANGLE%" || exit /b 1

if exist ".gclient" goto :configured
echo ==^> .gclient
call vpython3 scripts\bootstrap.py || (popd & exit /b 1)
:configured

rem Most of ANGLE's DEPS is for its tests and its Vulkan and GL backends; a D3D11
rem renderer wants none of it and gclient checks out everything it lists.
echo ==^> DEPS
call vpython3 "%REPO%\runtime\win\trim-angle-deps.py" DEPS third_party/catapult third_party/dawn third_party/llvm/src third_party/SwiftShader third_party/VK-GL-CTS/src || (popd & exit /b 1)

echo ==^> gclient sync ^(several minutes the first time^)
call gclient sync -f -D -R || (popd & exit /b 1)

rem Chromium's build names the SDK it compiles against and hands that version to
rem vcvarsall, so an older SDK fails later with a message about the version. Say it
rem here, before gn, and say how to add it.
set "WANT="
for /f "tokens=3 delims= '" %%v in ('findstr /b /c:"SDK_VERSION = " build\toolchain\win\setup_toolchain.py') do set "WANT=%%v"
if defined WANT (
    if not exist "%ProgramFiles(x86)%\Windows Kits\10\Include\!WANT!" (
        echo ANGLE builds with Windows SDK !WANT!, which is not installed
        echo Add it in the Visual Studio Installer, Individual components, and run again
        popd & exit /b 1
    )
    echo    Windows SDK !WANT!
)

popd
exit /b 0

rem ---------------------------------------------------------------------------------

:build
pushd "%ANGLE%" || exit /b 1

rem gn reads its arguments from the out folder, which keeps the quoting of
rem target_cpu="x64" out of this script and leaves the exact arguments beside the build.
if not exist "%OUT%" mkdir "%OUT%"
(
    echo target_cpu="%ARCH%"
    echo angle_build_all=false
    echo is_debug=false
    echo angle_has_frame_capture=false
    echo angle_enable_gl=false
    echo angle_enable_vulkan=false
    echo angle_enable_wgpu=false
    echo angle_enable_null=false
    echo use_siso=false
) > "%OUT%\args.gn"

echo ==^> gn gen %OUT%
call gn gen "%OUT%" || (popd & exit /b 1)
echo ==^> autoninja libEGL libGLESv2
call autoninja -C "%OUT%" libEGL libGLESv2 || (popd & exit /b 1)

popd
exit /b 0

rem ---------------------------------------------------------------------------------

:stage
if not exist "%STAGE%" mkdir "%STAGE%"
for %%f in (libEGL.dll libGLESv2.dll d3dcompiler_47.dll) do (
    if exist "%ANGLE%\%OUT%\%%f" (
        copy /y "%ANGLE%\%OUT%\%%f" "%STAGE%\%%f" >nul || exit /b 1
    ) else (
        if /i "%%f"=="d3dcompiler_47.dll" (
            echo    no d3dcompiler_47.dll in the build: D3D11 will use the one in System32
        ) else (
            echo The build made no %%f
            exit /b 1
        )
    )
)
copy /y "%ANGLE%\LICENSE" "%STAGE%\LICENSE" >nul || exit /b 1
> "%STAGE%\commit.txt" echo %PIN%
copy /y "%ANGLE%\%OUT%\args.gn" "%STAGE%\gn-args.txt" >nul || exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------------

:imports
rem Every DLL here has to import Windows DLLs only: the zips carry no Visual C++ runtime
rem and no Vulkan loader, and a DLL that wanted one would fail on a player's PC instead
rem of here. dumpbin comes with Visual Studio; without it the check is skipped, loudly.
set "DUMPBIN="
set "VS="
for /f "delims=" %%i in ('""%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"') do set "VS=%%i"
if not defined VS goto :no_dumpbin
for /d %%d in ("%VS%\VC\Tools\MSVC\*") do set "MSVC=%%~d"
if exist "%MSVC%\bin\Hostx64\x64\dumpbin.exe" set "DUMPBIN=%MSVC%\bin\Hostx64\x64\dumpbin.exe"
if not defined DUMPBIN goto :no_dumpbin

set "BAD="
for %%f in (libEGL.dll libGLESv2.dll) do (
    echo    %%f imports:
    dumpbin /nologo /dependents "%STAGE%\%%f" | findstr /r /c:"^    .*[.]dll$"
    dumpbin /nologo /dependents "%STAGE%\%%f" | findstr /i /c:"VCRUNTIME" /c:"MSVCP140" /c:"vulkan-1.dll" /c:"vk_swiftshader.dll" >nul
    if not errorlevel 1 set "BAD=%%f"
)
if not defined BAD exit /b 0
echo %BAD% imports a DLL the zips do not carry: the C runtime is not static, or a
echo backend that was meant to be off is on
exit /b 1

:no_dumpbin
echo WARNING: no dumpbin under Visual Studio: the DLL imports were not checked
exit /b 0
