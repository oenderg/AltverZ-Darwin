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
if "%3"=="" (set OUT=%STAGE%\out) else (set OUT=%~3)
if not exist "%OUT%" mkdir "%OUT%"

set ISCC=%ProgramFiles%\Inno Setup 7\ISCC.exe
if not exist "%ISCC%" set ISCC=%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe
if not exist "%ISCC%" (
    echo build-installer: Inno Setup 7 is not installed ^(ISCC.exe^)
    exit /b 2
)

"%ISCC%" /Qp /DStageDir="%STAGE%" /DOutDir="%OUT%" /DAppVersion="%VER%" /DSetupIcon="%~dp0..\pvz2.ico" "%~dp0pvz2.iss"
if errorlevel 1 exit /b 1
echo built %OUT%\PvZ2Reflourished-%VER%-windows-setup.exe
