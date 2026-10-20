; VR EyeCapture -- Inno Setup 6 script (built by scripts\package.ps1, which passes AppVersion, StageDir and OutDir).
;
; Installs into C:\Program Files\VR EyeCapture by default (the user can pick another folder), with one UAC prompt:
;   - the files of the stage folder (the program, steamvr\openvr_api.dll, the commented config.ini template, README, licences)
;     plus installed.txt, the marker that makes the program keep the user's data outside the read-only program folder
;     (config %APPDATA%\VR EyeCapture, logs and lists %LOCALAPPDATA%\VR EyeCapture, recordings <Videos>\VR EyeCapture);
;   - a Start menu entry; the OpenXR layer registration (EyeCaptureTray.exe --register, HKLM, while the installer is elevated):
;     the 64-bit layer (eye_capture.dll) in the 64-bit registry view and, for 32-bit OpenXR games, the 32-bit layer
;     (eye_capture32.dll + XR_APILAYER_JRDEVO_eye_capture32.json) in the 32-bit view (HKLM\SOFTWARE\WOW6432Node\Khronos\...;
;     tray/install.cpp). Its recordings are made by EyeCaptureRecorder.exe (64-bit), which the tray starts per 32-bit game;
;   - a "Start VR EyeCapture with Windows" task (on by default): EyeCaptureTray.exe --autostart on / off as the signed-in
;     user (start_with_windows in that user's config.ini + the HKCU Run value, the same switch as in the app window);
;   - "Start VR EyeCapture now" at the end (as the signed-in user, not elevated).
; Uninstall (Windows Settings > Apps, or the tray menu "Remove VR EyeCapture from this PC..."):
;   EyeCaptureTray.exe --quit (closes the running tray of this folder), --uninstall-cleanup (the original openvr_api.dll back
;   in every SteamVR game the user enabled, HKCU Home value, Start with Windows Run value, HKLM layer registrations: 64-bit
;   and 32-bit view), the
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

[Tasks]
; recording needs the tray running before the game starts; the app window's "Start with Windows" is the same switch
Name: "autostart"; Description: "Start VR EyeCapture with Windows (recording only works while it runs)"

[Run]
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--register"; StatusMsg: "Registering VR EyeCapture for OpenXR games..."; Flags: runhidden waituntilterminated
; as the signed-in user (not elevated): that user's config.ini (start_with_windows) and HKCU Run value
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--autostart on"; Tasks: autostart; Flags: runhidden waituntilterminated runasoriginaluser
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--autostart off"; Tasks: not autostart; Flags: runhidden waituntilterminated runasoriginaluser
Filename: "{app}\EyeCaptureTray.exe"; Description: "Start VR EyeCapture now"; Flags: postinstall nowait runasoriginaluser skipifsilent

[UninstallRun]
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--quit"; Flags: runhidden waituntilterminated; RunOnceId: "QuitTray"
Filename: "{app}\EyeCaptureTray.exe"; Parameters: "--uninstall-cleanup"; Flags: runhidden waituntilterminated; RunOnceId: "Cleanup"

[Code]
// After the UAC prompt the elevated setup window could open BEHIND Explorer (Windows' foreground lock: it is a new
// process). When its first page shows, it is lifted to the top (topmost on / off: allowed for an own window without the
// foreground right) and then asked for the focus (BringToFrontAndRestore).
function SetWindowPos(Wnd: HWND; After: HWND; X, Y, CX, CY: Integer; Flags: UINT): BOOL;
  external 'SetWindowPos@user32.dll stdcall';

var
  RaisedOnce: Boolean;

procedure CurPageChanged(CurPageID: Integer);
begin
  if RaisedOnce then Exit;
  RaisedOnce := True;
  // HWND_TOPMOST = -1, HWND_NOTOPMOST = -2; SWP_NOMOVE or SWP_NOSIZE or SWP_SHOWWINDOW = $0043
  SetWindowPos(WizardForm.Handle, -1, 0, 0, 0, 0, $0043);
  SetWindowPos(WizardForm.Handle, -2, 0, 0, 0, 0, $0043);
  BringToFrontAndRestore;
end;

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
