# Rename plan: OpenXR-EyeCapture -> VR EyeCapture

Why: the app will record SteamVR (OpenVR) games too, so "OpenXR" in the name is too narrow. "VR" is the word people search
for; "XR" would also promise AR/MR passthrough, which the app does not record.

When: in the same release as SteamVR recording v1 (planned as v0.5.0 "VR EyeCapture, now with SteamVR games"). The images
and the Settings header changed early (2026-10-17) so YouTube and GitHub can use them now.

## Names

| What | Old | New |
|---|---|---|
| Display name (UI, docs, images) | OpenXR-EyeCapture | **VR EyeCapture** |
| GitHub repo | JRDevo/OpenXR-EyeCapture | **JRDevo/VR-EyeCapture** (GitHub redirects the old URL) |
| Release zip | OpenXR-EyeCapture-vX.Y.Z.zip | VR-EyeCapture-vX.Y.Z.zip |
| Tagline | Stereo 3D recorder for any OpenXR game | Stereo 3D recorder for VR games |

## Kept as they are (no user ever sees them; changing them breaks things)

- File names: `eye_capture.dll`, `EyeCaptureTray.exe`, `XR_APILAYER_JRDEVO_eye_capture.json`, layer name
  `XR_APILAYER_JRDEVO_eye_capture`. Keeping them means no re-registration (HKLM) and no UAC prompt for existing users.
- Named objects shared by the layer, the tray and the OBS plugin: `Local\OpenXR-EyeCapture-Status`, `-Command`,
  `-TrayAlive`, `-Live`. All three parts must agree; a game that still has the old DLL loaded would lose the tray.
- The tray's hidden message window title (scripts and tests use it to close the tray), test registry keys
  `HKCU\Software\OpenXR-EyeCapture-Test`, the OBS source id (saved OBS scenes reference it).
- The local folder `E:\OpenXR-EyeCapture` (the HKLM registration and the Claude memory path point at it).

## Changed, with migration

1. **Config folder** `%APPDATA%\OpenXR-EyeCapture\config.ini` -> `%APPDATA%\VR EyeCapture\config.ini`.
   `src/config.cpp`: look in the new folder, then the old one, then next to the DLL. The tray copies the old file to the
   new folder once at start (never deletes the old one).
2. **Start with Windows** Run value `OpenXR-EyeCapture` -> `VR EyeCapture` (`tray/install.cpp` kRunValue). The tray no longer
   migrates it at start (antivirus rule, docs/DEVELOPMENT.md): ticking **Start with Windows** in Settings writes the new
   value and deletes the old one; at start a value pointing elsewhere only gets a balloon. `tests\run_install.ps1` covers it.
3. **All visible strings**: tray tooltip, menu ("Open VR EyeCapture"), balloons, message boxes, Settings window title and
   About link (`tray/tray.cpp`, `tray/settings.cpp`, `tray/postprocess.cpp`, `tray/input.cpp`), OBS source display name
   (`obs/plugin.cpp`), MP4 `sv3d` metadata_source (`src/st3d.cpp`).
4. **Docs and repo files**: README.md, docs/*.md, CLAUDE.md, .github/* (issue forms, SECURITY), obs/README.md,
   scripts/package.ps1 (zip name), CI badge URL, config.ini comments.
5. **Tests**: update the strings the tests look for (tests\run_settings.ps1, settings_test.cpp, install_test.cpp,
   run_install.ps1); all test scripts pass, 0 warnings.

## Steps the user does (Claude cannot or must ask first)

1. GitHub: Settings -> rename the repo to `VR-EyeCapture` (or say "rename the repo" and Claude runs `gh repo rename`).
2. GitHub: Settings -> Social preview -> upload `docs/images/social_preview.png`.
3. YouTube: upload the new `_thumb_v2.jpg` thumbnails; change "OpenXR-EyeCapture" in video descriptions and links.
4. After the repo rename: `git remote set-url origin https://github.com/JRDevo/VR-EyeCapture.git`.

## Done early (2026-10-17)

- `scripts/make_logo.py`: wordmark, banner, thumbnail overlay/template, new `social_preview()`.
- Settings window header (logo text + tagline).
- New YouTube thumbnails `captures\*_thumb_v2.jpg`.
