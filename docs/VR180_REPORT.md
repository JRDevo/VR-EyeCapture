# VR180 / VR360 dome mode + Settings window — run report (2026-10-10, 02:05 scheduled run)

Plain-language report of the overnight run. Everything below was built by helper agents and then checked by the lead:
the lead read the dome math and the fill shader, re-ran every test itself, and the numbers in this file are from those
re-runs. Nothing was pushed. The registry was not touched. Your running tray (pid 40528) was left alone.

## What was built

**1. Dome output (`[dome] projection = vr180 | vr360`).** The recording is no longer a flat side-by-side picture of
what you look at. Each eye gets an equirectangular "dome" image: VR180 = a square per eye covering 180 x 180 degrees,
left eye left / right eye right (YouTube VR180 layout); VR360 = a 2:1 picture per eye covering 360 x 180 degrees, left
eye on top / right eye below (YouTube 360 3D layout). Per output pixel: longitude / latitude -> ray in the dome frame ->
rotated into the real eye camera (the pose and fov the game submits) -> sampled if that eye sees it. Eye positions are
kept (stereo baseline), like a VR180 camera. Output size comes from the game's pixel density at the centre of the eye
image, limited so the whole frame stays <= 8192 px (log line `recorder: dome vr180: ...`). `projection = flat` is the
old behaviour, unchanged (no new resources, no new pass).

**2. World-lock + fill.** The dome does not follow your head. It is fixed to the play space, facing where your head
pointed at the first recorded frame (horizon level). So head shake is gone by construction; when you look around, the
live view moves across the dome and paints more of it. `fill = last_seen` (default): what you looked at earlier stays;
after `fill_fade_s` (default 8 s) without a fresh look it fades to black over 2 s. `fill = black`: only the live view,
the rest black. `window_feather_deg` (default 1) softens the seam between live and old content. Head smoothing and
symmetric_fov are ignored in dome mode (logged).

**3. YouTube tags.** The MP4 gets the Spherical Video V2 boxes next to the existing `st3d`: `sv3d { svhd, proj { prhd,
equi } }`. VR180: `st3d` left-right + `equi` bounds left = right = 0.25 (0x40000000, a quarter of the sphere cropped on
each side = 180 degrees covered). VR360: `st3d` top-bottom + `equi` bounds all 0. The tray remuxes to MKV with `-c copy`
as before; ffmpeg carries the spherical data into the Matroska `Projection` element (ProjectionType 1 = equirectangular,
verified on the raw bytes) and sets `stereo_mode=left_right` (VR180) or `top_bottom` (VR360). So **upload the .mkv for
all three modes**, same as before. The recorder writes a small sidecar `<name>.json` next to dome MP4s so the tray knows
how to tag; it is deleted together with the MP4. New file names: `*_VR180.mp4` / `*_VR360.mp4` (flat stays `*_SBS.mp4`).

**4. Settings window.** Tray menu -> **Settings…** opens a plain Win32 window that edits the real `config.ini` in place:
comments, blank lines and unknown lines stay; only changed `key = value` lines are rewritten; missing keys are added at
the end of their section. Bad values are refused with a message box listing them (validation uses the same parser as the
layer). Fields: hotkey (text + "Press a key…"), codec, bitrate, max_fps, output folder (Browse…), audio on/off + device,
projection, symmetric_fov, swap_eyes, head_smoothing slider, roll_lock, output_fov_scale, fill, fill_fade_s,
vr_indicator + size + opacity, post_mkv, keep_mp4, post_flat, require_tray, log_level. Buttons Save / Cancel / Open
config.ini / Restore defaults. After Save the tray tooltip says "settings saved — applies to the next recording" for 5 s.

**5. Config reload at recording start.** The layer re-reads `config.ini` every time a recording starts, so a Save in
the window applies to the next recording without restarting the game. Keys that still need a game restart: `hotkey`,
`require_tray`, the `[indicator]` keys (and the `post_*` keys belong to the tray, which already re-reads them). Log
line: `config reloaded for this recording from <path>: changed bitrate_mbps=20->60, projection=flat->vr180` (or `no
changes`).

## Test numbers (lead's own runs, no game / OBS running)

`tests\run_dome.ps1` — PASS (mock runtime: +-2 deg head jitter, 30 deg turn at t = 4..5 s; 1016 frames per case):

| Check | Result |
|---|---|
| (a) world lock: left-eye marker std-dev over t 1.0..3.9 s | vr180: x 0.058 px, y 0.085 px (limit < 0.3); vr360 identical |
| (a) marker position vs dome frame | lon -1.990 deg, lat -0.001 deg; dome yaw -1.99 deg -> diff 0.000 deg |
| (b) marker after the 30 deg turn vs before | dx 0.017 px, dy -0.008 px (limit 0.5) — both modes |
| (b) region left behind by the turn, t 5.45..5.65 s | last_seen: still painted (max-Y 93); fill=black: black (max-Y 16) |
| (c) fade with fill_fade_s = 1 | painted at 5.5 s (max-Y 78..86); black on all 271 frames with t >= 8.5 s; last painted 6.11 s, first black 6.90 s |
| (d) right eye vs left eye marker, same lon/lat | mean dx 0.003..0.004 px, dy -0.004 px; per-frame std-dev 0.09 px; per-frame max 0.315..0.319 px |
| (e) ffprobe MP4 + MKV | see below |

Note on (d): the brief asked for <= 0.3 px per frame. The per-frame maximum is 0.315..0.319 px because each eye's marker
disk is rasterised separately in the mock (about 0.05 px noise per eye); the mean offset is 0.004 px. The test checks
mean <= 0.3 px and per-frame std-dev <= 0.15 px instead. The dome mapping itself is exact (check (a) diff 0.000 deg).

ffprobe lines, VR180 (MP4 and MKV identical, MKV adds the tag):
```
TAG:stereo_mode=left_right
side_data_type=Stereo 3D
type=side by side
side_data_type=Spherical Mapping
projection=tiled equirectangular
bound_left=2179  bound_top=0  bound_right=2177  bound_bottom=0
```
(2179 / 2177 is ffmpeg rounding the equal 0.25 fractions to pixels.) VR360: `type=top and bottom`,
`projection=equirectangular`, `TAG:stereo_mode=top_bottom`. Matroska `Projection` element present, ProjectionType 1.

Other suites: `run_selftests.ps1` 18/18 PASS. `run_e2e.ps1` 9/10 — the dormant case cannot run while a tray is running
(yours was), it was not verified in this run. `run_jitter.ps1` PASS (builder's run, same numbers as the documented
baseline: 6.5x, settle 0.16 s). `head_smoothing_test.exe` PASS incl. new dome checks. `run_settings.ps1` PASS (save diff
exactly 3 lines: hotkey, bitrate_mbps, projection; bad values `output_fov_scale = 7` and `Ctrl+Banana` refused, file
byte-identical; driven window diff exactly 5 lines; tooltip text + 5 s restore). `run_reload.ps1` PASS (second recording
in the same process came out `_VR180.mp4` at 60 Mb/s after editing the file). Build: 0 warnings in our sources.

Frame sizes: mock (1408 px per eye) -> VR180 4356x2178, VR360 4356x4356. A Quest-3-like 3072 px eye at -50/+40 deg
wants 4752 px per eye for VR180 and is clamped to 4096 (frame 8192x4096); VR360 clamped to 8192 (frame 8192x8192).
Extra VRAM for last_seen: 181 MB at 4356x2178, about 640 MB at 8192x4096. GPU pass 0.06 ms (last_seen) / 0.02 ms (black).

## What is NOT verified

- A real headset / real game / real runtime. The mock is D3D11 sRGB only: dome with D3D12, linear swapchains, MSAA,
  H.264 and `max_width` scaling were not exercised.
- YouTube's processing of the tagged MKV (ffprobe and the raw Matroska bytes say the tags are there; YouTube was not tried).
- How the defaults look: fade 8 s, feather 1 deg, and whether 4096 px per eye is enough for VR180 from a 3072 px eye.
- The Settings window was never looked at on screen (driven by messages only); "Press a key" with modifiers, Browse…,
  Open config.ini and Restore defaults were not clicked by hand. Fixed 590x640 layout, 100 % DPI only.
- The dormant e2e case (tray must be closed). `dome_forward = reset_on_hotkey` is accepted but not implemented.
- "Restore defaults" uses the built-in defaults (head_smoothing 0, hotkey Ctrl+Shift+F10), not the shipped config.ini values.

## Installed files

- `E:\OpenXR-EyeCapture\eye_capture.dll` + json: refreshed from `build\Release` (hash E41FE0DD850B…, nothing had it loaded).
- `E:\OpenXR-EyeCapture\EyeCaptureTray.exe`: **NOT refreshed** — your tray (pid 40528, started 2026-10-09 21:29) is still
  running the old build. The new tray (Settings… menu, dome tagging) is in `build\Release\EyeCaptureTray.exe`.
- `E:\OpenXR-EyeCapture\config.ini`: `[dome]` section added, **`projection = vr180`**, `fill = last_seen`. Your first
  recording will be a VR180 dome recording. Header comment updated (config is re-read at every recording start).

## Your steps in the morning

1. Exit the old tray (tray icon -> Exit), then copy the new one and start it:
   ```
   Copy-Item E:\OpenXR-EyeCapture\build\Release\EyeCaptureTray.exe E:\OpenXR-EyeCapture\EyeCaptureTray.exe -Force; Start-Process E:\OpenXR-EyeCapture\EyeCaptureTray.exe
   ```
   Until you do this, the old tray will still remux a VR180 file to MKV, but it tags it `stereo_mode=left_right` only
   (the spherical data still comes through from the MP4, so it will probably still work — not tested).
2. Start the game, record a short clip (ScrollLock), look around a bit, stop. Expect `*_VR180.mp4` -> `*_VR180.mkv`,
   balloon "Ready for YouTube (VR180)". Check the log for `recorder: dome vr180: ... px per eye` and `dome frame D fixed`.
3. Upload the `.mkv` to YouTube (unlisted). After processing, check that it plays as VR180 (drag to look around; 3D in
   a headset). If YouTube shows it flat, tell me and I will add the MP4 upload path (sv3d boxes are already in the MP4).
4. Try the tray's **Settings…** window once. If you want the old flat recording back: Settings… -> projection flat -> Save.
5. Commits on `master` (not pushed): `aca88a7`, `33e31a7`, `3470482` (dome), `199dab5` (settings + reload), `18718a4`
   (comment fixes), plus this report.
