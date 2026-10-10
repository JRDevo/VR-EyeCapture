# OpenXR-EyeCapture — working rules for Claude sessions

Stereo recorder for any OpenXR game: an implicit OpenXR API layer (`eye_capture.dll`) + a tray app
(`EyeCaptureTray.exe`). Read `README.md`, `docs/DEVELOPMENT.md` and `docs/VR180_REPORT.md` first.
Project memory lives in the Claude workspace for this folder (`~/.claude/projects/E--OpenXR-EyeCapture/memory`).

## Build / test
- CMake is NOT on PATH. Use:
  `"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release`
- 0 warnings in our sources is the bar. Tests: `tests\run_selftests.ps1`, `tests\run_e2e.ps1`, `tests\run_jitter.ps1`,
  `tests\run_dome.ps1`, `tests\run_settings.ps1`, `tests\run_reload.ps1`, plus the unit-test exes in `build\Release`.
- NEVER run GPU/encoder tests while `amtrucks.exe`, `eurotrucks2.exe` or `obs64.exe` run (`Get-Process` first) —
  it stutters the user's VR stream. Test scripts set `XR_EYE_CAPTURE_REQUIRE_TRAY=0` and `XR_EYE_CAPTURE_TRAY_IPC=0`
  so a running tray does not convert test files.
- ffmpeg/ffprobe: `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg.Essentials_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.1-essentials_build\bin\`.

## Install rules
- The game loads the ROOT copies: `E:\OpenXR-EyeCapture\eye_capture.dll` + json + `EyeCaptureTray.exe`.
  Refresh them from `build\Release` after a build — the DLL only when no game has it loaded, the tray only when
  no `EyeCaptureTray` process runs (close it, copy, start it again).
- Registry: the layer is registered in **HKLM only** (`scripts\install.ps1`, needs UAC). ETS2/ATS use their own
  built-in OpenXR loader that reads only HKLM. Never also register HKCU — the official loader then loads the layer
  twice and crashes.

## Git
- Branch `master`, remote `origin` = GitHub `JRDevo/OpenXR-EyeCapture` (public). Commit with `-c user.name=JRDevo -c user.email=42043842+JRDevo@users.noreply.github.com`. Push only when the user asks.

## User setup facts
- Quest 3 via Virtual Desktop, runtime VDXR; ATS/ETS2 OpenXR beta (D3D11). Hotkey `ScrollLock` (Alt combos open the
  window menu; Space is a game key; Ctrl+F10 / Shift+F11 belong to the DLAA mod).
- YouTube ignores the MP4 `st3d` box; the tray remuxes to `.mkv` with Matroska tags — the user uploads the `.mkv`.
- The layer is dormant unless the tray runs (`require_tray = 1`). Logs: `logs\eye_capture.log`, `logs\tray.log`.
