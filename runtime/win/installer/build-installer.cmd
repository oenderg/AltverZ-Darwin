@echo off
rem Build the Windows setup program from a staged tree, from a normal prompt:
rem
rem   runtime\win\installer\build-installer.cmd <staged folder> [version] [output folder]
rem
rem The staged folder is what runtime/win/installer/build-installer.sh writes (arm64\ and
rem x64\; its stage.zip unpacks to it). The compiler is Inno Setup 7, installed per PC from
rem its official release: https://jrsoftware.org/isdl.php
setlocal
set STAGE=%~1
if "%STAGE%"=="" (
    echo usage: build-installer.cmd ^<staged folder^> [version] [output folder]
    exit /b 2
)
if "%2"=="" (
    for /f "usebackq delims=" %%v in ("%STAGE%\version.txt") do set VER=%%v
) else (
    set VER=%2
)
if not exist "%STAGE%\game-version.txt" (
    echo build-installer: no game-version.txt in %STAGE% ^(build-installer.sh writes it^)
    exit /b 2
)
for /f "usebackq delims=" %%g in ("%STAGE%\game-version.txt") do set GAMEVER=%%g
rem The mod's names: profile.txt, written by build-installer.sh, has one NAME=value per line.
if not exist "%STAGE%\profile.txt" (
    echo build-installer: no profile.txt in %STAGE% ^(build-installer.sh writes it^)
    exit /b 2
)
for /f "usebackq tokens=1,* delims==" %%a in ("%STAGE%\profile.txt") do set %%a=%%b
if "%3"=="" (set OUT=%STAGE%\out) else (set OUT=%~3)
if not exist "%OUT%" mkdir "%OUT%"

set ISCC=%ProgramFiles%\Inno Setup 7\ISCC.exe
if not exist "%ISCC%" set ISCC=%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe
if not exist "%ISCC%" (
    echo build-installer: Inno Setup 7 is not installed ^(ISCC.exe^)
    exit /b 2
)

"%ISCC%" /Qp /DStageDir="%STAGE%" /DOutDir="%OUT%" /DAppVersion="%VER%" /DGameVersion="%GAMEVER%" /DAppName="%AppName%" /DAppExe="%AppExe%" /DAppUrl="%AppUrl%" /DAppUserModelID="%AppUserModelID%" /DArtifact="%Artifact%" /DDataFolder="%DataFolder%" /DSetupIcon="%~dp0..\pvz2.ico" "%~dp0pvz2.iss"
if errorlevel 1 exit /b 1
echo built %OUT%\%Artifact%-%GAMEVER%-Darwin-%VER%-Windows-Setup.exe
