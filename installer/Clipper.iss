; Inno Setup script for Clipper (https://jrsoftware.org/isinfo.php).
; Built by scripts\package-windows.ps1:
;   ISCC /DAppVersion=1.0.0 /DAppVersionNumeric=1.0.0 /DSourceDir=<staged folder> /DOutputDir=dist
;        /DOutputName=Clipper-1.0.0-windows-x64-setup installer\Clipper.iss
; Per user, no admin: a clipboard manager belongs to the person logged in (history, hotkey, start at login).

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef AppVersionNumeric
  #define AppVersionNumeric AppVersion
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\Clipper-" + AppVersion + "-windows-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#ifndef OutputName
  #define OutputName "Clipper-" + AppVersion + "-windows-x64-setup"
#endif

[Setup]
AppId={{60AA8DFB-1337-4B3F-B461-CF1AC6613A55}
AppName=Clipper
AppVersion={#AppVersion}
AppVerName=Clipper {#AppVersion}
AppPublisher=Kimscode Studio
AppPublisherURL=https://github.com/KimsFerdy/Clipper
AppSupportURL=https://github.com/KimsFerdy/Clipper/issues
AppUpdatesURL=https://github.com/KimsFerdy/Clipper/releases
AppCopyright=Copyright (c) 2026 KimsFerdy
VersionInfoVersion={#AppVersionNumeric}
DefaultDirName={autopf}\Clipper
DefaultGroupName=Clipper
DisableProgramGroupPage=yes
DisableDirPage=auto
LicenseFile=..\LICENSE
SetupIconFile=..\assets\clipper.ico
UninstallDisplayIcon={app}\Clipper.exe
UninstallDisplayName=Clipper
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\Clipper.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\Clipper"; Filename: "{app}\Clipper.exe"; Comment: "Clipboard history"
Name: "{autodesktop}\Clipper"; Filename: "{app}\Clipper.exe"; Tasks: desktopicon

[Registry]
; Clipper writes its own start-at-login value on first run (and removes it when turned off in Preferences).
; this only makes sure it's gone after uninstalling
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Clipper"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\Clipper.exe"; Description: "{cm:LaunchProgram,Clipper}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\Clipper.exe"; Parameters: "--quit"; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "QuitClipper"

[Code]
// an update: close the running Clipper first, its exe can't be replaced while it runs
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  if FileExists(ExpandConstant('{app}\Clipper.exe')) then
  begin
    Exec(ExpandConstant('{app}\Clipper.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Sleep(500);
  end;
  Result := '';
end;

// history and settings are the user's, ask before throwing them away
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
    if MsgBox('Also delete your clipboard history and Clipper settings?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(ExpandConstant('{localappdata}\Clipper'), True, True, True);
      DelTree(ExpandConstant('{userappdata}\Clipper'), True, True, True);
    end;
end;
