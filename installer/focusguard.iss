; SPDX-License-Identifier: AGPL-3.0-or-later
; FocusGuard - Windows installer (Inno Setup 6). CI builds it from the same
; folder that goes into the zip:
;
;   iscc /DAppVersion=1.0.1 /DPackageDir=<package folder> /DOutputDir=<folder> installer\focusguard.iss
;
; It installs for the current user only, with no admin prompt, into
; %LOCALAPPDATA%\Programs\FocusGuard. A running FocusGuard is closed before
; upgrading or uninstalling (so it saves its history) and started again after
; an upgrade. The "start at login" entry is the same one the settings app's
; switch uses, so the two always agree.

#ifndef AppVersion
  #error Pass the version: iscc /DAppVersion=x.y.z
#endif
#ifndef PackageDir
  #error Pass the folder with the programs: iscc /DPackageDir=...
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

; Never change this: it is how Windows recognises an installed FocusGuard.
#define AppGuid "6CB0E64C-D512-4753-A20E-F5CA8611DBB9"

[Setup]
AppId={{{#AppGuid}}
AppName=FocusGuard
AppVersion={#AppVersion}
AppVerName=FocusGuard {#AppVersion}
AppPublisher=FocusGuard
AppPublisherURL=https://github.com/DylanBeMe/FocusGuard
AppSupportURL=https://github.com/DylanBeMe/FocusGuard/issues
AppUpdatesURL=https://github.com/DylanBeMe/FocusGuard/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\FocusGuard
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=..\data\focusguard.ico
UninstallDisplayIcon={app}\focusguard-settings.exe
UninstallDisplayName=FocusGuard
OutputDir={#OutputDir}
OutputBaseFilename=focusguard-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Offered on a fresh install only; after that the settings app's switch decides.
Name: "autostart"; Description: "Start FocusGuard when I log in"; Check: not IsUpgrade
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\FocusGuard"; Filename: "{app}\focusguard-settings.exe"; Comment: "See and change what FocusGuard lets take your keyboard focus"
Name: "{autodesktop}\FocusGuard"; Filename: "{app}\focusguard-settings.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "FocusGuard"; ValueData: """{app}\focusguard-tray.exe"""; Tasks: autostart

[Run]
Filename: "{app}\focusguard-tray.exe"; Check: ShouldStartFocusGuard; Flags: nowait
Filename: "{app}\focusguard-settings.exe"; Description: "Open FocusGuard Settings"; Flags: nowait postinstall skipifsilent

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  WindowClass = 'FocusGuardWindow';                 // see src/focusguard_win.cpp
  InstanceMutex = 'Local\FocusGuard.SingleInstance';
  WM_CLOSE = $0010;

var
  WasRunning: Boolean;

function IsUpgrade: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + '{#AppGuid}' + '}_is1');
end;

// Asks a running FocusGuard to quit, as its tray menu's Exit does, and waits
// up to five seconds for it. Returns whether it was running.
function StopFocusGuard: Boolean;
var
  Wnd: HWND;
  I: Integer;
begin
  Wnd := FindWindowByClassName(WindowClass);
  Result := Wnd <> 0;
  if not Result then
    Exit;
  PostMessage(Wnd, WM_CLOSE, 0, 0);
  for I := 1 to 50 do begin
    if not CheckForMutexes(InstanceMutex) then
      Break;
    Sleep(100);
  end;
end;

function ShouldStartFocusGuard: Boolean;
begin
  Result := WasRunning or WizardIsTaskSelected('autostart');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  WasRunning := StopFocusGuard;
  if CheckForMutexes(InstanceMutex) then
    Result := 'FocusGuard is still running. Exit it from its tray icon, then try again.'
  else
    Result := '';
end;

function InitializeUninstall: Boolean;
begin
  StopFocusGuard;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
begin
  // Remove the start-at-login entry if it starts this copy of FocusGuard.
  if (CurUninstallStep = usUninstall) and RegQueryStringValue(HKCU, RunKey, 'FocusGuard', Command) and
     (Pos(Lowercase(ExpandConstant('{app}')), Lowercase(Command)) > 0) then
    RegDeleteValue(HKCU, RunKey, 'FocusGuard');
end;
