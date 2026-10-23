# VR EyeCapture — working rules for Claude sessions

Stereo 3D recorder for VR games (renamed from OpenXR-EyeCapture, see `docs/RENAME_PLAN.md`): an implicit OpenXR API layer
(`eye_capture.dll`), a per-game SteamVR proxy (`openvr_api.dll`, `src/openvr/`, `docs/STEAMVR_DESIGN.md`) + a tray app
(`EyeCaptureTray.exe`). Read `README.md`, `docs/DEVELOPMENT.md` and `docs/VR180_REPORT.md` first.
Project memory lives in the Claude workspace for this folder (`~/.claude/projects/E--VR-EyeCapture/memory`).

## Build / test
- CMake is NOT on PATH. Use:
  `"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release`
- 32-bit OpenXR games (`docs/X86_DESIGN.md`): a second configure `cmake -B build32 -S . -G "Visual Studio 17 2022" -A Win32`
  builds only `eye_capture32.dll` (+ its 32-bit mock runtime / `layer_e2e`); `EyeCaptureRecorder.exe` (the 64-bit helper that
  records for it) is in the x64 build. Test: `tests\run_x86.ps1` (GPU, same skip rules as `run_e2e.ps1`).
- 0 warnings in our sources is the bar. Tests: `tests\run_selftests.ps1`, `tests\run_e2e.ps1`, `tests\run_jitter.ps1`,
  `tests\run_dome.ps1`, `tests\run_settings.ps1`, `tests\run_reload.ps1`, `tests\run_yt8k.ps1` (tray 8K step, CPU only),
  `tests\run_tag3d.ps1` (tray "Tag a video as 3D" via `--tag3d`, CPU only), `tests\run_live.ps1` (OBS live share, GPU),
  `tests\run_steamvr.ps1` (SteamVR proxy `openvr_api.dll` + `mock_openvr.dll` + fake game `openvr_e2e.exe`, and the quick-mode `EyeCaptureMirror.exe` cases, GPU; also not
  while `Crisis VRigade 2`, `vrserver` or `vrcompositor` runs), the CPU exes `openvr_hook_test.exe` / `steamvr_install_test.exe`,
  `tests\run_steamvr_ui.ps1` (the tray's "SteamVR games" window on a fake game tree, CPU only),
  `tests\run_vrinput.ps1` (EyeCaptureVrInput.exe, the SteamVR controller hotkey, on the mock + status_sim, CPU only),
  `tests\run_install.ps1` (registration:
  `install_test.exe` + the tray's `--register/--unregister/--status` and GUI flow on test keys under
  `HKCU\Software\OpenXR-EyeCapture-Test`; no admin, run it NON-elevated), plus the unit-test exes in `build\Release`.
- NEVER run GPU/encoder tests while `amtrucks.exe`, `eurotrucks2.exe` or `obs64.exe` run (`Get-Process` first) —
  it stutters the user's VR stream. Test scripts set `XR_EYE_CAPTURE_REQUIRE_TRAY=0` and `XR_EYE_CAPTURE_TRAY_IPC=0`
  so a running tray does not convert test files.
- ffmpeg/ffprobe: `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg.Essentials_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.1-essentials_build\bin\`.

## Install rules
- The game loads the ROOT copies: `E:\VR-EyeCapture\eye_capture.dll` + json + `EyeCaptureTray.exe` (the tray's
  "SteamVR games" copies `steamvr\openvr_api.dll`, else `build\Release\openvr_api.dll`, into a game folder on Enable).
  Refresh them from `build\Release` after a build — the DLL only when no game has it loaded, the tray only when
  no `EyeCaptureTray` process runs (close it, copy, start it again).
- 32-bit games: the root also holds `eye_capture32.dll` + `XR_APILAYER_JRDEVO_eye_capture32.json` (from `build32\Release`) and
  `EyeCaptureRecorder.exe`; the 32-bit json is registered in the 32-bit view (`HKLM\SOFTWARE\WOW6432Node\Khronos\...`) by the
  same `--register` / `install.ps1`. Refresh `EyeCaptureRecorder.exe` only when none runs (it lives as long as its 32-bit game).
- Registry: the layer is registered in **HKLM only**, written by `EyeCaptureTray.exe --register` running elevated (the
  tray offers it at start / from its menu with one UAC prompt; `--unregister` removes it; `tray/install.cpp`). ETS2/ATS
  use their own built-in OpenXR loader that reads only HKLM. Never also register HKCU — the official loader then loads
  the layer twice and crashes (the tray removes HKCU copies at every start). `scripts\install.ps1` / `uninstall.ps1`
  are dev-only (not in the zip); never run `--register` / `install.ps1` from a session without asking (UAC + HKLM).
- Installed copy (`C:\Program Files\VR EyeCapture`, from the setup): update it after a build with
  `scripts\deploy_installed.ps1` (one UAC prompt; also refreshes the proxy in enabled SteamVR games). Ask before running it.

## Git
- Branch `master`, remote `origin` = GitHub `JRDevo/VR-EyeCapture` (public; renamed from `JRDevo/OpenXR-EyeCapture`, which redirects). Commit with `-c user.name=JRDevo -c user.email=42043842+JRDevo@users.noreply.github.com`. Push only when the user asks.

## User setup facts
- Quest 3 via Virtual Desktop, runtime VDXR; ATS/ETS2 OpenXR beta (D3D11). Hotkey `ScrollLock` (Alt combos open the
  window menu; Space is a game key; Ctrl+F10 / Shift+F11 belong to the DLAA mod).
- YouTube ignores the MP4 `st3d` box; the tray remuxes to `.mkv` with Matroska tags — the user uploads the `.mkv`.
- The layer is dormant unless the tray runs (`require_tray = 1`). Logs: `logs\eye_capture.log`, `logs\tray.log`.
