; VR EyeCapture -- Inno Setup 6 script (built by scripts\package.ps1, which passes AppVersion, StageDir and OutDir).
;
; Installs into C:\Program Files\VR EyeCapture by default (the user can pick another folder), with one UAC prompt:
;   - the files of the stage folder (the program, steamvr\openvr_api.dll, the commented config.ini template, README, licences)
;     plus installed.txt, the marker that makes the program keep the user's data outside the read-only program folder
;     (config %APPDATA%\VR EyeCapture, logs and lists %LOCALAPPDATA%\VR EyeCapture, recordings <Videos>\VR EyeCapture);
;   - a Start menu entry; the OpenXR layer registration (EyeCaptureTray.exe --register, HKLM, while the installer is elevated);
;   - "Start VR EyeCapture now" at the end (as the signed-in user, not elevated).
; Uninstall (Windows Settings > Apps, or the tray menu "Remove VR EyeCapture from this PC..."):
;   EyeCaptureTray.exe --quit (closes the running tray of this folder), --uninstall-cleanup (the original openvr_api.dll back
;   in every SteamVR game the user enabled, HKCU Home value, Start with Windows Run value, HKLM layer registration), the
;   files; then two questions, both No by default: delete the settings and logs? delete the recordings in Videos\VR EyeCapture?
; An update installs over the old version: the running tray is closed first (--quit of the installed copy).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir is required (scripts\package.ps1 passes it)
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{681E6863-ACB3-4811-BFE0-E99EAF4DD40D}
AppName=VR EyeCapture
AppVersion={#AppVersion}
AppVerName=VR EyeCapture {#AppVersion}
AppPublisher=JRDevo
AppPublisherURL=https://github.com/JRDevo/VR-EyeCapture
AppSupportURL=https://github.com/JRDevo/VR-EyeCapture/issues
AppUpdatesURL=https://github.com/JRDevo/VR-EyeCapture/releases
AppCopyright=Copyright (c) 2026 JRDevo
DefaultDirName={autopf}\VR EyeCapture
DefaultGroupName=VR EyeCapture
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutDir}
OutputBaseFilename=VR-EyeCapture-Setup-v{#AppVersion}
SetupIconFile=..\tray\app.ico
UninstallDisplayIcon={app}\EyeCaptureTray.exe
UninstallDisplayName=VR EyeCapture
LicenseFile=..\LICENSE.md
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; the installer closes the tray itself (EyeCaptureTray.exe --quit); the Restart Manager cannot close a tray-only app
CloseApplications=no
; the uninstaller runs elevated as the signed-in user: the per-user folders below are that user's
UsedUserAreasWarning=no
VersionInfoVersion={#AppVersion}
VersionInfoCompany=JRDevo
VersionInfoProductName=VR EyeCapture
VersionInfoDescription=VR EyeCapture setup

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\VR EyeCapture"; Filename: "{app}\EyeCaptureTray.exe"; Comment: "Stereo 3D recorder for VR games"

[Run]
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--register"; StatusMsg: "Registering VR EyeCapture for OpenXR games..."; Flags: runhidden waituntilterminated
Filename: "{app}\EyeCaptureTray.exe"; Description: "Start VR EyeCapture now"; Flags: postinstall nowait runasoriginaluser skipifsilent

[UninstallRun]
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--quit"; Flags: runhidden waituntilterminated; RunOnceId: "QuitTray"
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--uninstall-cleanup"; Flags: runhidden waituntilterminated; RunOnceId: "Cleanup"

[Code]
// An update: close the tray of the installed copy before its files are replaced.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Tray: String;
  Code: Integer;
begin
  Result := '';
  Tray := ExpandConstant('{app}\EyeCaptureTray.exe');
  if FileExists(Tray) then
    Exec(Tray, '--quit', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code);
end;

// Inno Setup has no constant for the Videos folder: ask Windows (CSIDL_MYVIDEO, follows a moved Videos folder). '' = unknown.
function SHGetFolderPath(Owner: HWND; Csidl: Integer; Token: THandle; Flags: DWORD; Path: String): HResult;
  external 'SHGetFolderPathW@shell32.dll stdcall';

function UserVideosDir(): String;
var
  Buf: String;
  Z: Integer;
begin
  Result := '';
  Buf := StringOfChar(#0, 1024);
  if SHGetFolderPath(0, $000E, 0, 0, Buf) = 0 then
  begin
    Z := Pos(#0, Buf);
    if Z > 0 then Result := Copy(Buf, 1, Z - 1) else Result := Buf;
  end;
end;

// After the files are gone: the user's own data, only on a Yes (No is the default; nothing is asked in a silent uninstall).
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Videos: String;
begin
  if (CurUninstallStep <> usPostUninstall) or UninstallSilent then Exit;
  if DirExists(ExpandConstant('{userappdata}\VR EyeCapture')) or DirExists(ExpandConstant('{localappdata}\VR EyeCapture')) then
    if MsgBox('Also delete your VR EyeCapture settings and logs?' #13#10#13#10 +
              ExpandConstant('{userappdata}\VR EyeCapture') + #13#10 + ExpandConstant('{localappdata}\VR EyeCapture') + #13#10#13#10 +
              'Choose No to keep them for a later install.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(ExpandConstant('{userappdata}\VR EyeCapture'), True, True, True);
      DelTree(ExpandConstant('{localappdata}\VR EyeCapture'), True, True, True);
    end;
  Videos := UserVideosDir();
  if Videos = '' then Exit;
  Videos := Videos + '\VR EyeCapture';
  if DirExists(Videos) then
    if MsgBox('Also delete your recordings?' #13#10#13#10 + Videos + #13#10#13#10 +
              'This deletes every video in that folder. Choose No to keep them.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(Videos, True, True, True);
end;
