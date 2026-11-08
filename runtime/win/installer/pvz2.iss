; PvZ2: Reflourished for Windows: the setup program.
;
; One setup for both PC architectures, made with Inno Setup 7 from a staged tree
; (runtime/win/installer/build-installer.sh stages it; build-installer.cmd runs this
; script on Windows). It installs the ARM64 program on ARM64 Windows and the x64 one
; everywhere else, per user and without an administrator prompt, into
; %LOCALAPPDATA%\Programs\PvZ2 Reflourished.
;
; The data folders are not part of the install: %APPDATA%\PvZ2 Reflourished (saves and
; settings) and %LOCALAPPDATA%\PvZ2 Reflourished (the OBB, cache and log) are made by
; the game. Uninstalling removes the program and the second folder, then asks about the
; first (default No, so a reinstall keeps the saves); a silent uninstall keeps them.
;
; WinSparkle runs this program with /SILENT /SP- /NOICONS when it installs an update
; (sparkle:installerArguments in the Windows appcast); a silent update starts the game
; again at the end.

#ifndef StageDir
  #error StageDir must name the staged tree (arm64\ and x64\)
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef OutDir
  #define OutDir "."
#endif
; The icon is runtime/win/pvz2.ico beside the .iss's folder; a compile from a copied
; tree passes /DSetupIcon=<absolute path>.
#ifndef SetupIcon
  #define SetupIcon "..\pvz2.ico"
#endif

#define AppName "PvZ2 Reflourished"
#define AppExe "PvZ2 Reflourished.exe"
#define AppUrl "https://github.com/oenderg/Reflourished-Darwin"

[Setup]
; A fixed identity: players' saves, settings and the update check hang off it. Never change it.
AppId={{8DA0E954-1137-4C7C-85DF-40BD39EC232A}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=oenderg
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\PvZ2 Reflourished
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible arm64
ArchitecturesInstallIn64BitMode=x64compatible arm64
OutputDir={#OutDir}
OutputBaseFilename=PvZ2Reflourished-{#AppVersion}-windows-setup
SetupIconFile={#SetupIcon}
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
; The game is a 3D window; the wizard's pages are ordinary. No admin, no reboot.
DisableWelcomePage=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
; Inno cannot count the architecture's files for the Dir page's estimate (they are picked
; by a Check function at run time), so name both builds' real sizes instead.
DiskSpaceMBLabel=The ARM64 build needs about 66 MB of free space; the Intel/AMD build about 230 MB.

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
; The ARM64 program on ARM64 Windows, the x64 one elsewhere. Neither tree has a
; "portable" file: the installed build keeps its data in %APPDATA% and %LOCALAPPDATA%.
Source: "{#StageDir}\arm64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "portable"; Check: IsArm64
Source: "{#StageDir}\x64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "portable"; Check: not IsArm64

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; The finished page, for a person installing by hand.
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent
; A silent update (WinSparkle) starts the new version again, so the player is back in
; the game; a silent first install starts nothing.
Filename: "{app}\{#AppExe}"; Flags: nowait; Check: RestartAfterSilentUpdate

[UninstallDelete]
; The program folder (the uninstaller removes itself afterwards), and the folder the
; game can make again: the OBB, the x64 translation cache, the CDN archive and the log.
Type: filesandordirs; Name: "{app}"
Type: filesandordirs; Name: "{localappdata}\PvZ2 Reflourished"

[Code]
const
  CrLf = #13#10;

var
  PreviousInstall: Boolean;

function InitializeSetup(): Boolean;
begin
  Result := True;
end;

// The app folder is not available in InitializeSetup; by the install step it is,
// whether the folder is the default or one the player chose.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    PreviousInstall := FileExists(ExpandConstant('{app}\{#AppExe}'));
end;

function RestartAfterSilentUpdate: Boolean;
begin
  Result := PreviousInstall and WizardSilent;
end;

{ Inno's own free-space estimate cannot see the architecture's files (a Check
  function chooses them), so check the real need here, before anything is written. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  FreeBytes, TotalBytes, Needed: Int64;
begin
  Result := '';
  NeedsRestart := False;
  if IsArm64 then Needed := 66 * 1024 * 1024 else Needed := 230 * 1024 * 1024;
  if GetSpaceOnDisk64(ExtractFileDrive(ExpandConstant('{app}\')), FreeBytes, TotalBytes) and
     (FreeBytes < Needed) then
    Result := 'Not enough free disk space: this build needs about ' +
              IntToStr(Needed div (1024 * 1024)) + ' MB.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  // WinSparkle's own state (the last check, a skipped version) is this app's, not the
  // player's; it goes with the program. The saves and the settings are asked about below.
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\oenderg\PvZ2 Reflourished');
end;

{ The program folder and the game's data folder are gone by now (they are in
  UninstallDelete); this is the "then" of asking about the one folder that holds
  the player's saves, and the default is to keep them. A silent uninstall keeps
  them without asking. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Answer: Integer;
  Saves: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  Saves := ExpandConstant('{userappdata}\PvZ2 Reflourished');
  if UninstallSilent then
  begin
    Log('Silent uninstall: the saves and settings stay in ' + Saves);
    exit;
  end;
  Answer := MsgBox('Also delete your saves and settings?' + CrLf + CrLf +
    'They are in ' + Saves + ': the game''s saves, its options, and the keys you chose.' + CrLf + CrLf +
    'Choose No to keep them, so that installing the game again continues where you left off.',
    mbConfirmation, MB_YESNO or MB_DEFBUTTON2);
  if Answer = IDYES then
  begin
    DelTree(Saves, True, True, True);
    Log('Uninstall: the saves and settings were deleted from ' + Saves);
  end
  else
    Log('Uninstall: the saves and settings stay in ' + Saves);
end;
