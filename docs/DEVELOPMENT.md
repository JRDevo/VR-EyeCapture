# OpenXR-EyeCapture — development notes

Internals, build and tests. For installing and using it, see the top-level [`README.md`](../README.md).
Companion doc: [`VULKAN.md`](VULKAN.md) (phase 2 plan).

## Build

Visual Studio 2022 (MSVC, x64) and its bundled CMake. The first configure fetches the OpenXR SDK (git + internet).

```
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Targets: `eye_capture.dll` + `XR_APILAYER_JRDEVO_eye_capture.json` (the layer), `EyeCaptureTray.exe`, and the tests
`encoder_selftest.exe`, `layer_loadtest.exe`, `mock_runtime.dll` (+ `mock_runtime.json`), `layer_e2e.exe`, and the
CPU-only unit tests (`settings_test.exe`, `install_test.exe`, `head_smoothing_test.exe`, `indicator_test.exe`, ...).
`live_share_test.exe` is the live share consumer test (`tests\run_live.ps1`). Optional: `-DEYE_BUILD_OBS_PLUGIN=ON`
adds the OBS source plugin `obs-eyecapture.dll` (fetches the libobs 31.0.0 headers only; `obs/README.md`).
`scripts\package.ps1` builds Release and zips the release files into `dist\`: `OpenXR-EyeCapture-v<version>.zip`
(layer + tray) and a separate `OpenXR-EyeCapture-OBS-v<version>.zip` (`obs-eyecapture.dll` + `obs\README.md`; upload
both as release assets; the OBS plugin is never in the main zip). Our sources build with `/W3 /permissive-`
and zero warnings; `/EHa` everywhere (see "Fault handling").

`scripts\install.ps1` run from the repo copies `build\Release\{eye_capture.dll, json, EyeCaptureTray.exe}` to the repo
root (next to `config.ini`, git-ignored) and registers that json, so the repo root has the same layout as the release
zip. Re-run it after every build you want the games to pick up (a running game keeps the DLL locked).
`install.ps1` / `uninstall.ps1` are developer scripts and are not in the release zip: users run `EyeCaptureTray.exe`,
which registers the layer itself (see `tray/install.h/.cpp` below; same rules as the script) with one UAC prompt.

**Art.** `scripts\make_logo.py` (Python 3 + Pillow, developer only) regenerates every logo file in `docs\images` from
code: the mark (`logo_mark_512.png`, `logo_mark_128.png`, `logo_icon_800.png` on a dark rounded square for a YouTube /
GitHub avatar), `logo_wordmark.png`, the README header `logo_banner.png`, and `thumbnail_overlay.png` (1280 x 720 RGBA:
"3D" badge, logo, dark band at the bottom) which `scripts\make_thumbnail.ps1` puts over a frame of a recording with
ffmpeg `overlay` + `drawtext`; `thumbnail_template.png` shows that overlay on a placeholder picture. `logo_mark.svg` and
`logo.svg` are the same drawing as hand-written vectors (same coordinates as the script). Colours: left eye blue
`#3b8bff`, right eye coral `#ff6b6b`, recording dot `#ff3b3b`, background `#0e1017` (the social preview's). The zip
ships `make_thumbnail.ps1` with the overlay next to it as `scripts\thumbnail_overlay.png` (the script looks there first,
then in `docs\images`).

## Source map

| File | What |
|---|---|
| `src/layer.cpp` | Negotiation, `xrCreateApiLayerInstance`, `xrGetInstanceProcAddr` dispatch, all hooks, layout detection, frame clock, start logic |
| `src/frame_source.h/.cpp` | `IFrameSource` (graphics-API side of a capture), format helpers, private encoder device creation |
| `src/source_d3d11.cpp` | D3D11 games: ring created on the encoder device, opened on the game device; fences created on the game device |
| `src/source_d3d12.cpp` | D3D12 games: ring = D3D12 shared committed resources opened on the encoder device; D3D12 shared fences |
| `src/source_vulkan.cpp` | Stub (logs "not yet supported") |
| `src/recorder.h/.cpp` | Recording: ring bookkeeping, video processor RGBA→NV12, linear→sRGB pre-pass, head-smoothing re-projection pass or the dome pass, MF sink writer, AAC, finalize, st3d (+ sv3d and the `<name>.json` sidecar in dome mode). Rate control (`rate_control` / `qp` / `quality_vs_speed`, `CreateWriter`): vbr = `eAVEncCommonRateControlMode_UnconstrainedVBR` + `AVEncCommonMeanBitRate`, cbr = `_CBR` + mean bitrate, qp = `_Quality` + `CODECAPI_AVEncVideoEncodeQP` (UINT64, the QP in the default / I / P / B 16-bit fields; H.264 also gets `CODECAPI_AVLowLatencyMode`, without which the NVIDIA H.264 MFT keeps all 4 NV12 pool surfaces and stalls after 4 frames; the MP4 then has an `avc3` sample entry, which `PatchSt3d` and `verify_mp4.ps1` accept); an encoder that refuses that (the NVIDIA AV1 MFT: `MF_E_INVALIDMEDIATYPE`, and `E_INVALIDARG` through `ICodecAPI`) gets a second attempt with `CODECAPI_AVEncCommonQuality = 100 - qp * 100 / 51` (never both: with both set the HEVC MFT takes the quality value), then the existing "quality vs speed only" / "encoder defaults" fallbacks. `quality_vs_speed` 0..100 = `CODECAPI_AVEncCommonQualityVsSpeed` for every codec; `auto` keeps the AV1 > 1 Gpixel/s = 25 rule. `encoder reports: ...` logs what the encoder MFT returns through `ICodecAPI::GetValue` (rate control mode, mean bitrate, QP, quality vs speed). The rate control as applied (`rcText`, e.g. `VBR 120 Mb/s`, `constant QP 24`, `constant quality 53 (from qp 24; this encoder takes no QP)`) is in the `sink writer ready` line and the summary. `constant_frame_rate`: see "Timestamps and A/V sync". |
| `src/overlay.h/.cpp` | `record_overlays`: shared types (`OverlayLayer` (kind quad / cylinder / equirect / equirect2 / cube, behind, family, colour scale / bias) / `OverlayFrame` game thread -> recorder, `OverlayDraw` per queue item, `OverlayEye`), `TypelessForDxgi` / `OverlayFamily` / `OverlayFamilyFormat` (atlas format families), the CPU mesh + projection (`OverlayClipVertices`, `OverlayPixelBox`), the per-pixel ray of equirect / cube layers (`OverlayRayFor`, CPU mirror `OverlayRayLookup`; unit test `overlay_test.exe`), `OverlayHeadLock` (head-locked layers under head smoothing), `OverlayPack` (game thread: sub-images -> a ring slot's atlases) / `OverlayAtlasSet` (worker: the per-family atlases on demand + the compositor) shared by the recorder and the live share, and `OverlayCompositor` (encoder device: ring-slot RTVs, per-family atlas SRVs, shaders `psmain` / `psray` / `psover`, scratch copy of the destination, projection copy for layers behind it). See "Overlays (record_overlays)" |
| `src/indicator.h/.cpp` | In-headset recording indicator: state tracker (+ the live state, `indicator_live`), 64x64 straight-alpha picture, the quad layer's swapchain / VIEW space (D3D11) |
| `src/head_smoothing.h/.cpp` | Head-motion smoothing, CPU side: orientation filter (follow, catch-up, roll lock, edge guard) and the per-eye re-projection parameters; quaternion helpers |
| `src/dome.h/.cpp` | VR180 / VR360 dome, CPU side: `[dome]` settings, output size (density, rounding, 8192 clamp), lon / lat -> ray, dome frame D (yaw only), per-eye matrix, direction -> equirect (tests). The GPU pass (`kDomePs`) is in `recorder.cpp` |
| `src/audio_capture.h/.cpp` | WASAPI loopback → continuous 48 kHz stereo int16 on the QPC clock |
| `src/st3d.h/.cpp` | Post-finalize MP4 patch: `st3d` box (+ `sv3d` for dome recordings) into the video sample entry |
| `src/control.h/.cpp` | Control thread: hotkey, tray IPC, status publishing, beeps. `ControlPublishFinal()` hands a finalized recording's result ("saved <file>") to the status block synchronously from `xrDestroySession` / `xrDestroyInstance` (the 250 ms loop is stopped with the last armed session); a Stopping -> Idle transition is also published at once. At shutdown only `ownerPid` / `state` are cleared, `message` / `file` stay so the tray still reads "saved <file>" after the game is gone `beeps = 0` silences every cue (`Cue()` returns at once): the control thread copies the Config once at `ControlStart`, so `ReloadConfigForRecording` hands the reloaded value over with `ControlSetBeeps()` (an atomic; logged `control: beeps on|off (config reload)` when it changes) before the start cue can play. |
| `src/live_share.h/.cpp`, `src/live_share_proto.h` | Live share (OBS plugin) producer (raw eyes, or the recorder's re-projection + overlays on its own share device and worker; Full / Half size) + the shared-memory protocol (v3); see "Live share (OBS)" |
| `obs/live_consumer.h/.cpp`, `obs/plugin.cpp`, `obs/obs.def` | Live share consumer (no libobs; used by the plugin and `tests/live_share_test.cpp`) and the OBS source plugin `obs-eyecapture.dll` (`obs/README.md`) |
| `src/shared_status.h` | Layer ↔ tray IPC names + layout (shared memory `Local\OpenXR-EyeCapture-Status`, event `...-Command`, tray-alive mutex `...-TrayAlive`) |
| `src/config.h/.cpp`, `src/log.h/.cpp` | `config.ini`, log files. `ParseConfigFile(path, cfg, problems)` = `LoadConfig`'s parser that also collects the "bad value" messages (used by the app window to validate a candidate file). The layer re-reads the file at every recording start: `ReloadConfigForRecording()` in `layer.cpp` (called from `CaptureImpl` before the `swap_eyes` decision, under `g_lock`) copies output_dir / codec (`VideoCodec`, `src/video_codec.h`: hevc / h264 / av1) / bitrate_mbps / rate_control (`RateControl`, `src/video_codec.h`: vbr / cbr / qp) / qp / quality_vs_speed (`int`, -1 = auto) / fps / max_fps / constant_frame_rate / max_width / gamma / swap_eyes / record_overlays / audio / beeps (also handed to the control thread, `ControlSetBeeps`) / `head` / `dome` / log_level over `g_cfg` after re-applying the `XR_EYE_CAPTURE_OUTPUT_DIR` and `XR_EYE_CAPTURE_CONFIG` overrides, and logs one `config reloaded for this recording from <path>: changed k=old->new, ...` (or `no changes`) line; an unreadable file keeps the old values (warning). hotkey, require_tray, indicator and post_* keep their game-start values |
| `src/disk.h` | Low-space guard + free-space text, header only (`disk::FreeBytes` = `GetDiskFreeSpaceExW` on the path or its nearest existing parent, `DriveName`, `SizeText`, `FreeText` = "E: 812 GB free"). `MinFreeToStart()` = 10 GB: `Recorder::Impl::Run` does not start a recording below it (message `not started: E: has only .. free (needs 10 GB; ...)`, state Failed -> the tray balloons "Recording not started"), and `Processor::RunJob` does not start a conversion below it PLUS the estimated output on the MP4's drive (checked once the ffprobe duration is known: duration x the video bitrates of the steps it still makes / 8 -- MKV re-encode 150 Mbit/s, MKV remux = the MP4's size, 8K = `yt8k_bitrate_mbps`, flat 40 Mbit/s; no duration: a re-encode counts as the MP4's size; logged `job: disk: .. free, needs about ..`; `job: NOT STARTED <file>: low disk space -- E: has only .. free; this conversion needs about N GB (about .. of output + 10 GB to spare)`, notice "Not enough disk space", MP4 kept). `FreeBytes` keeps the trailing separator on a drive root (`E:\`) and a UNC share root (`\\server\share\`), which `GetDiskFreeSpaceExW` needs. `MinFreeToContinue()` = 2 GB: the recorder's worker checks every 2 s and calls `Stop()` below it (normal finalize; message `saved <file> -- stopped: E: almost full (.. free)`; the tray's "Recording saved" balloon becomes "Recording stopped: disk almost full"; tray.cpp matches ` almost full` OR ` -- stopped`, because the 160-character message can cut the reason after a long path, and a cut message -- not ending in `free)` -- is replaced by the fixed text "Recording stopped: the output drive is almost full."). Tests: `EYECAP_TEST_MIN_FREE_GB` / `EYECAP_TEST_KEEP_FREE_GB` override the thresholds (run_hold G; encoder_selftest by hand). Investigation (no temp data on C:): the MF MPEG-4 sink writes mdat then moov straight into the output file (no `MF_MPEG4SINK_MOOV_BEFORE_MDAT`, no temp file); `st3d` patches in place or writes `<file>.st3d.tmp` next to the file; every ffmpeg output is next to the MP4 and the encoders are single-pass (no x265 stats files); the only `%TEMP%` use is the app window's small config candidate file. |
| `src/hotkey.h/.cpp` | The `hotkey` value: a comma-separated list of bindings, any one toggles. `Key` (keyboard + mouse buttons, every VK except Escape / modifiers / Windows keys; unnamed ones as `Key0xNN`; exact Ctrl / Shift / Alt), `Pad` (`Pad:A`, `Pad2:LB+RB`; XInput bits + LT / RT + Guide), `Joy` (`Joy:<device or *>:Button1..128+POVUp/Down/Left/Right`, device = case-insensitive substring of the sanitised DirectInput product name). `ParseHotkey` refuses the whole list if one binding is bad; `Hotkey.vk/ctrl/shift/alt` = the first Key binding (0 = none), `Hotkey.text` = canonical list. One naming table: `KeyBindingText` / `PadBindingText` / `JoyBindingText` (the app window's `HotkeyText` and the tray's capture use them). The layer (`control.cpp` `HotkeyDown`) polls only the Key bindings; the tray fires Pad / Joy |
| `tray/input.h/.cpp` | The tray's controller / wheel hotkeys. One poll thread (20 ms): XInput loaded at run time (xinput1_4 -> 1_3 -> 9_1_0, Guide via ordinal 100 when present; empty slots probed once a second), DirectInput 8 (`DI8DEVCLASS_GAMECTRL` attached-only, XInput devices skipped by `IG_` in `DIPROP_GUIDANDPATH`, `c_dfDIJoystick2`, `DISCL_BACKGROUND \| DISCL_NONEXCLUSIVE` on a hidden top-level window that also gets `WM_DEVICECHANGE` -> re-enumerate 1 s later; every 10 s while a Joy binding's device is missing; a device failing reads for ~2 s is dropped until the next enumeration). A binding fires on the rising edge of all its buttons held (300 ms debounce) -> `tray.cpp` `OnInputFire` -> `ECC_TOGGLE` (`tray: input <text> pressed -> toggle` / `... but no game is running`). Capture mode (the window's "Capture...", `--inputs`): buttons held at arm time are ignored until released; the chord = buttons held on ONE device just before the first release. The tray reloads the bindings at start, after Save in the window and when config.ini's write time changes (500 ms timer). `tray: devices: ...` is logged at start and on every change. `EyeCaptureTray.exe --inputs [s]` lists devices and prints each press |
| `tray/tray.cpp` | `EyeCaptureTray.exe`: tray icon / menu, status polling, queues finished recordings (`message` = `saved <file>` in the status block; the first read is only a baseline so older recordings are not converted), `logs\tray.log`. Fallback scan (game went away, and every 10 s while not recording): `*_SBS.mp4` / `*_VR180.mp4` / `*_VR360.mp4` in the captures folder that are newer than the tray start, have no `.mkv`, are not open for writing, and have a stable size for 3 s are queued (`tray: fallback scan queued <file>`); both paths dedupe by full path |
| `tray/install.h/.cpp` | Layer registration, registry only (no UI, no logging): `Query(exeDir)` -> `Registered` / `NotRegistered` / `StalePath` (a value with our json file name registered from another folder) / `DuplicateHkcu`, plus `hkcuDuplicate` and `filesPresent` (json + dll next to the exe). A value is "ours" when the last path component of its name equals `XR_APILAYER_JRDEVO_eye_capture.json` (case-insensitive, `\` or `/`), as in `install.ps1`; paths compare after `GetFullPathName` + lower case; ours with a non-zero DWORD counts as not registered. `Register` writes `<exeDir>\json = 0` to HKLM and deletes every other same-name value there (stale folder, case variant), then re-reads to verify; it opens the key for writing only when something must change, so an already-correct registration needs no admin. `Unregister` deletes every same-name HKLM value. `RemoveHkcuDuplicates` deletes same-name values from the CURRENT user's HKCU (no admin). Win32 error out (`ERROR_ACCESS_DENIED` -> "needs administrator rights", `ERROR_FILE_NOT_FOUND` -> files missing). 64-bit registry view. `SetTestRoots` redirects both roots for tests. Tray side (`tray.cpp`): CLI `--register` / `--unregister` (`RunRegCli`: log to `tray.log`, also cleans that account's HKCU; exit 0 / 1 / 5 = access denied), `--status` appends `registered=yes\|no\|stale` (+ `hkcu_duplicate=yes`, `files=missing`); GUI start: `RemoveHkcuDuplicates` + `Query`, then `WM_APP+4` -> `StartupRegCheck` (files missing -> error balloon, no prompt; not registered / stale -> `RegisterFlow` question). `RegisterFlow` / `UnregisterFlow`: `MessageBoxW` question -> `Elevate()` = `ShellExecuteExW` `runas` on its own exe with `--register` / `--unregister`, waits (pumping messages, 120 s cap) and reads the exit code; UAC declined (`ERROR_CANCELLED`) -> balloon; non-zero -> error box naming `logs\tray.log`; success -> `RemoveHkcuDuplicates` in the non-elevated parent (the elevated child may be another account) + `Query` + balloon. Cached `g_reg`, re-queried every 10 s in the timer, when the menu opens and after an action; `g_regBusy` blocks the menu while a question / UAC prompt is open. Not registered -> first tooltip line "Not registered for VR games (right-click: Register)" and the menu item "Register for VR games..." (else "Unregister from VR games..."), above Exit. The registration flow is off in the `EYECAP_TRAY_TEST_SHM` test mode unless `EYECAP_TRAY_TEST_REG=1`; in test mode questions are logged and answered No (no dialog, no UAC) |
| `tray/settings.h/.cpp` | (app window -> Export: an owner-drawn live preview (`ID_LAYOUTPREVIEW`, example Quest 3 recording 8192x4630 scaled by "Resolution") draws the recording, the crop and the output frame with its pixel sizes from `pp::PredictLayout` (postprocess.h, the same pure arithmetic as `PredictFrame` / the filter graph; `settings_test layout` checks it). The "YouTube output" drop-down drives the hidden `post_mkv` / `post_yt8k` checks. Export rows (10, `kExportRows`): YouTube output, Convert, 8K bitrate, 8K codec, "Options:" 2D check (these five sit left of the preview, so their texts must fit `ecw`), Keep original MP4, **Output layout** (`ID_LAYOUT3D`, moved here from Stereo 3D, one row above Eye crop and below the preview because its items are wider than `ecw`; a normal Combo field bound to `layout_3d` -- its `Field` section stays `[dome]` -- items by index: `tb_half`, `sbs_half`, `sbs_full`, `tb_full`), Eye crop, **Record directly in the YouTube layout (no re-encode)** (`ID_RECORDYT`, appended to the id enum, Check field bound to `record_for_youtube` in `[capture]`), the two conversion buttons. Both columns are 17 rows high; the left one's spare 30 px goes to the Stereo 3D group (`leftEx`), so the window stays 1156x999. `UpdateLayoutEnabled` (on `SetControls` = load and Restore defaults, and on the projection combo's `CBN_SELCHANGE`): Output layout + its label and the record check are greyed unless Projection is flat; **Dome fill / Fade after** (combo, edit, their three labels in `g_domeLabels`) are greyed unless it is VR180 / VR360. `UpdateYt8kEnabled` also greys the 8K bitrate / codec (+ labels) while the record check is ticked on a flat projection (`RecordForYoutubeOn`; refreshed on its `BN_CLICKED`). With the record check ticked the layout preview shows "Headset view" (the native example 8192x4630, crop hatched) -> "Recording": the frame `PredictLayout(4096, 4630, layout, crop, 4320)` then capped like the recorder's `FitOutputSize` (max_width = frame width, then the codec limit, uniform, even), with the lines "Recording: WxH <layout>", "(uploaded as is after a copy)" and the per-eye size; no separate 8K line.) The tray's app window (title "OpenXR-EyeCapture"; left-click on the tray icon) (plain `CreateWindowExW` controls on a popup window, nested message loop so the tray keeps running) and the config writer behind it. `SaveKeyValuesToFile` edits `ConfigPath()` in place: only `key = value` lines of known keys whose canonical value changed are rewritten (spelling, spacing, trailing comment, quotes, BOM and CR/LF kept); a missing non-default key is appended at the end of its `[section]` (section created if absent); the candidate is written to a temp file and checked with `ParseConfigFile` (only NEW problems block the save; `hotkey` is checked with `ParseHotkey` first); then temp-file + `MoveFileEx`. Errors go through one `Reporter` (MessageBox; in the tray's `EYECAP_TRAY_TEST_SHM` mode they are logged instead). `HotkeyText` maps a VK + modifiers to a name `ParseHotkey` accepts. Tray side: `Open OpenXR-EyeCapture` menu item (bold default), `WM_APP+3` opens the window (tests post it), `OnSettingsSaved` (tooltip override for 5 s via `kTipTimer`, `tray.log` line). Help: `kHelp[]` (control id -> text) feeds one `TOOLTIPS_CLASS` window (`TTF_IDISHWND \| TTF_SUBCLASS`, labels carry their control's id in `GWLP_USERDATA` and have `SS_NOTIFY`) and the read-only "Description" edit (`ID_HELPTEXT`), updated from `WM_SETCURSOR` + a 100 ms hover timer, `EN/CBN/BN_SETFOCUS` and `CBN_SELCHANGE` of the projection combo; combo `Field`s have optional display `labels` (the file still gets the `options` values, mapped by index) The indicator group has an owner-drawn preview (`ID_INDPREVIEW`, plain GDI, 50 ms pulse timer killed on close): a +-50 degree view circle (`kPreviewHalfFovDeg`) with the dot at atan2(x,-z) / atan2(y,-z); it re-reads the five indicator edits + the check on every change (unparsable text keeps the last good value) and never touches Save. **Conversions** (full-width group under the two columns, `ID_QSTATUS` three-line status -- file, `Format: <pp::FormatText>`, step / percent / ETA, each line cut to the control width with "..." by `FitText` (GetTextExtentPoint32) so nothing wraps or clips --, `ID_QPROGRESS` progress bar (`PBST_PAUSED` while paused), `ID_QPAUSE` Pause / Resume / Run now, `ID_QCANCEL`, `ID_QLIST` waiting list (`LBS_NOTIFY | LBS_EXTENDEDSEL`, `WS_HSCROLL` + `LB_SETHORIZONTALEXTENT` = the widest row, so long rows scroll instead of being cut; rows `<file>  -  <format>  -  <state>` in run order: forced jobs by `forceSeq`, then the rest oldest first; states next / queued (started by you) / waiting for you / waiting: converts <hold reason> / queued; jobs without a snapshot show the current `config.ini` format + "(current settings)"), `ID_QSTART` Start (every selected row), `ID_QSTARTALL` Start all, `ID_QREMOVE` (every selected row)) and the Export group's last row **Convert a recording...** (`ID_CONVERTPICK`, forced) / **Add to queue...** (`ID_ADDQUEUE`, held by the user); both pass `WindowFormat()` = the conversion keys of `CollectControls()` (what the window shows, saved or not; refused with a message when it makes nothing): `settings.cpp` is also linked into `settings_test` without the worker, so it never calls `pp::Processor` directly -- the tray hands it `settings::QueueHooks` (std::function: `state`, `queued`, `pickFiles(owner, fmt, start)`, `togglePause`, `cancel`, `start(id)`, `startAll()`, `remove(id)`) once at start (`InstallQueueHooks` in `tray.cpp`; `queued` fills `fmt` of jobs without a snapshot from `g_savedFmt`, reloaded with the input bindings). `UpdateQueueView` polls them on a 500 ms timer (`kQueueTimer`) and on `WM_APP+20` (`settings::NotifyQueueChanged`, posted from the tray's `WM_PP`) and only touches a control when its text / position / list content changed (no flicker). Run now = the running job is held by `post_when` and not user-paused: `start(state.runningId)` forces it. Start is enabled when a selected row is not forced yet, Start all when any row is not. The window is 1156x999 (fits 1080p with a taskbar; the existing scroll fallback covers smaller work areas). Text fit: `CheckTextFit()` runs once the window is filled and measures every visible label (DrawText `DT_CALCRECT | DT_WORDBREAK` at the control width vs its height), check box / button / group box (text width + box / padding) and every drop-down item (vs the closed combo minus arrow and edges) with the dialog font; each cut one is logged `settings: text does not fit: ...`, then `settings: text fit check: N controls, M cut` (`run_settings.ps1` requires M = 0; it caught "8K 3D MKV (higher quality, slower)", 182 px in 166). Combo drop-down lists are as wide as their widest item. Long combo labels were shortened, the detail is in the help text ("8K 3D MKV", "3D MKV + 8K 3D MKV", eye crop "16:9 (recommended)" / "None (full eye)"). The Description edit is `5 x tmHeight` tall and its formatting rectangle (`EM_SETRECTNP`) is a whole number of lines, so its last visible line is never cut in half. Output folder: **Browse...** = `IFileOpenDialog` with `FOS_PICKFOLDERS` (opened at the field's folder or its nearest existing parent), and a "Free space:" line (`ID_OUTFREE`, `disk::FreeText`, refreshed on `EN_CHANGE`; empty field = `<exe folder>/captures`; below 10 GB it says recordings and conversions will not start). "Bitrate" (`ID_BITRATE`) is a Combo field bound to `bitrate_mbps` (`0|150|200|250|300`, `Field::custom` "Custom (N Mbit/s)" like max_width) with an `ID_BITRATEHINT` line "about N GB per hour" (`(mbps*45+50)/100`; Default = 120, 80 with codec h264 or av1, with a "(H.264: 80 Mbit/s)" / "(AV1: 80 Mbit/s)" suffix). "Codec" (`ID_CODEC`) has hevc / h264 / av1. "8K bitrate" (`ID_YT8KBITRATE`, Export group) is bound to `yt8k_bitrate_mbps` (`100|150|200` + Custom) and greyed unless YouTube output makes the 8K file; "8K codec" (`ID_YT8KCODEC`, the row under it) is bound to `yt8k_codec` (`hevc|av1`) and greyed the same way (`UpdateYt8kEnabled`). Tray: a left click on the icon opens this window (`WM_LBUTTONUP` -> `OpenSettings`), right click / context menu shows the menu. Frame-rate warning: `pp::UnevenFor60(fps)` / `pp::FpsWarningText` (postprocess.h; 60 / 120 / 180 / 240 / 30 / 20 / 15 +-1.5 fps are even, tested by `settings_test layout`): the tray adds a tooltip line (dropped when the 127 characters are full) and one "Frame rate N fps" balloon per game process when a recording starts; the window shows the red `ID_FPSWARN` label on the Export group box edge via `QueueHooks::gameFps` (refreshed with the 500 ms queue timer). |
| `tray/postprocess.h/.cpp` (queue, per-job format) | **Jobs** (`Processor::Job {id, path, force, forceSeq, userHeld, ownFormat, fmt, doneMkv, doneFlat, doneYt8k}`): `pp::JobFormat` = the conversion keys `post_mkv`, `post_flat`, `post_yt8k`, `keep_mp4`, `layout_3d`, `eye_crop`, `yt8k_codec`, `yt8k_bitrate_mbps` (`FormatFromConfig` / `ApplyFormat` / `FormatText` "MKV + 8K AV1 100M + 2D, 4:3, tb_half, keep MP4", inline in the header because `settings_test` links `settings.cpp` without `postprocess.cpp`). `RunJob` loads `config.ini` and, for a job with `ownFormat`, applies the snapshot over it (every other key, e.g. `ffmpeg_path`, stays from `config.ini`); `runningFmt_` = the effective format (`State::format`, logged on `job: start ... format <text> (the job's own settings / config.ini at job start)`). Who snapshots: the window's "Add to queue..." / "Convert a recording..." (`WindowFormat()`, unsaved values included), tray menu "Convert a recording..." (`config.ini` now). No snapshot: automatic recordings and "Convert last recording now" (old behaviour: `config.ini` at job START, so a settings change while the recording waits for the game still applies, and "Convert last recording now" still upgrades the waiting automatic job). **Dedup** (`Enqueue(path, force, fmt*, userHeld)`): same path (case-insensitive) AND same effective format (`ownFormat ? fmt : config.ini now`; the running job compares `runningFmt_`); a forced request on a match upgrades it (`... was waiting for the hold -- forced, runs now`), else `... is already waiting / running`. **Picking** (`PickLocked`): the forced job with the lowest `forceSeq` (the order they were forced: Start = next, even behind other forced jobs), else -- not held -- the oldest job that is not `userHeld`. `userHeld` (Add to queue, every restored job) never starts on its own, also with `post_when = now`; `StartJob(id)` (waiting: force; running: `runningForced_`, resumed unless user-paused) and `StartAll()` (every non-forced waiting job, queue order) clear it. **keep_mp4 = 0**: before deleting, under `m_`, the waiting queue is searched for the same path (any format); if any, the MP4 and sidecar stay (`job: keep_mp4 = 0, but N more queued job(s) need X -- not deleted yet`), so the last job for an MP4 applies its own `keep_mp4`. **8K name**: `_AV1` after the crop tag when the job's `yt8k_codec = av1` (`<name>_YT8K[_4x3|_full]_AV1.mkv`); `encode()` records the encoder that succeeded (`usedEnc`) and when it is not `av1_nvenc` the verified `.part` gets the first free untagged name instead (`job: 8K step: made by X, not av1_nvenc -- renamed A to B`, A = the planned `_AV1` name). **`.part` outputs**: every step's ffmpeg writes `<final>.part` (with `-f matroska` / `-f mp4`), the verification (size, ffprobe duration) reads the `.part`, and `commit()` renames it to `FreePath(final)` (`MOVEFILE_WRITE_THROUGH`; a failed rename deletes the `.part` and fails the step); `DeleteStaleParts(mp4)` removes `<stem>{.|_| }*.mkv.part` / `*.mp4.part` siblings in `LoadQueue` (before the worker runs) and at each job's start; cancel / exit deletes the running step's `.part` (`deleted partial X.mkv.part`). **picker**: tray.cpp `PickMp4s` offers only `*.mp4`, `PickAndQueue` refuses names not ending in `.mp4` or ending in `_FLAT.mp4` (`... is not a recording -- not queued`, one balloon "Not a recording: a, b"); `RunJob` never deletes a source that is not `*.mp4`; `ReadSidecar` counts `<name>.json` only when it contains `"projection"`. **queue.json** (`<exeDir>\queue.json`, `SaveQueue` after every change: Enqueue, Start, StartAll, Remove, job start / end, each finished step; the snapshot is taken under `saveM_` + `m_`, written to `.tmp`, `FlushFileBuffers`'d and `MoveFileEx`'d; one job object per line `{"path":..,"own_format":0/1[,"done_mkv":"x.mkv","done_flat":..,"done_yt8k":..][, the 8 keys]}`; the running job first; `StepDone` fills `done_*` (output FILE name) after each verified step and freezes `runningFmt_` as the job's snapshot (`own_format 1`); a restored job skips a done step whose file still exists (`job: MKV step already done before the restart: x.mkv`; counts as ok for `keep_mp4`, named in the balloon) and redoes it when the file is gone; `RunJob` returns `interrupted` (aborted by `Stop()` before it completed) and only then the worker skips the final `SaveQueue`, so an interrupted conversion comes back and one that completed during Exit does not). `LoadQueue` in `Start()`: every entry whose MP4 exists is restored `userHeld` (logged `queue: restored X (<format>) -- waits for you`; the game / session that held it is gone, so nothing starts on its own after a restart), missing MP4s are dropped (logged, file rewritten). Not guarded against two trays in one folder (the single-instance mutex prevents that outside test mode). |
| `tray/postprocess.h/.cpp` | Post-processing worker thread (FIFO, one job at a time): ffmpeg lookup, reads the recorder's `<name>.json` sidecar (`ReadSidecar`; none = flat SBS; it counts only when it contains `"projection"`; `pp::Recording {projection, topBottom, hasSidecar, packed, packing, crop, width, height}`: `packed` = flat + `"youtube":1`, the record_for_youtube sidecar `{"projection":"flat","layout":"top_bottom","packing":"tb_half","eye_crop":"16:9","eyes":2,"width":7680,"height":4320,"youtube":1}`, `packing` from `"packing"` (else `layout` left_right -> sbs_half, otherwise tb_half), `crop` from `"eye_crop"`, `topBottom` from `"layout"` (missing: tb packings); `Name()` "YouTube tb_half" ...). **Packed recordings** (`fixedLayout = Dome() || packed`): the MKV step is always the plain remux (`stereo_mode` from the sidecar, `<name>.mkv` without crop tag, size verification, logged `job: MKV step: recorded in the YouTube layout (tb_half, 16:9, 7680x4320): remux only (-c copy) ... this job's layout_3d X / eye_crop Y do not apply`), the 8K step logs `... already in the YouTube 8K layout -- 8K step skipped` and counts as ok (no `_YT8K`, `keep_mp4 = 0` deletes MP4 + sidecar after the verified remux), the flat step first takes the left eye and un-squeezes it (`tb_half`: `crop=iw:ih/2:0:0,scale=iw:ih*2,setsar=1`; `tb_full`: `crop=iw:ih/2:0:0`; `sbs_half`: `crop=iw/2:ih:0:0,scale=iw*2:ih,setsar=1`; `sbs_full`: `crop=iw/2:ih:0:0`), then the usual 16:9 crop + 3840 scale; only-8K jobs on one make nothing ("Nothing to convert ... already in the YouTube layout"). MKV step (`layout_3d` sbs_full or a dome recording: plain `-c copy` remux, `stereo_mode=left_right`, `top_bottom` for VR360, verified by size > 90% of the MP4; any other `layout_3d`, the default `tb_half`, on a flat recording: re-encode with the same per-eye layout graph as the 8K step at NATIVE size (`sc = min(1, 8192/long side of the frame)`, no upscale, source fps kept, no 60 fps cap, hevc_nvenc 150/200 Mbit/s else libx265 crf 18, audio copied, tag per layout), verified by existence, size > 0 and the `ffprobe` container duration within max(1 s, 2%) of the MP4's (no ffprobe: logged, size only; unreadable duration: failure); the MP4 is only deleted after that), optional `_FLAT.mp4` (dome: `v360` flat view of the left eye), step 2b optional `_YT8K.mkv` (`post_yt8k`, flat SBS only, dome = logged skip: each eye `crop` -> `scale` with in-filter expressions (lanczos, no padding, even sizes) -> `vstack` / `hstack` (left eye first) + `setsar=1`, yuv420p; layout = `layout_3d` (`Layout3d`, read per job): `tb_half` (default; frame = one eye's aspect, each eye stored at full width x half height; YouTube reads 3D as half-format, a full SBS looked 2x stretched), `sbs_half` (half width x full height, `hstack`, `stereo_mode=left_right`), `sbs_full` (each eye at its own aspect, `hstack`, frame 2x eye aspect, `left_right`), `tb_full` (own aspect, `vstack`, frame eye aspect / 2, `top_bottom`); for all four the FRAME's short side is 4320 (`EYECAP_TRAY_TEST_YT8K_SHORT`) and its long side <= 8192: `sc = min(S/min(fw,fh), 8192/max(fw,fh))` with the frame size fw x fh in eye units (iw x ih; 2*iw x ih for sbs_full; iw x 2*ih for tb_full), stored eye = `2*round(iw*sc/(2 or 4))` x `2*round(ih*sc/(2 or 4))` (4 = the squeezed direction); `fps=60` in front when ffprobe (next to ffmpeg) reports `r_frame_rate` > 60; hevc_nvenc VBR `yt8k_bitrate_mbps` (default 150; `-maxrate` = (4b+1)/3, `-bufsize` = 2b, preset p5; logged as `job: 8K step: yt8k_bitrate_mbps N -> hevc_nvenc ...`) else libx265 crf 18; `yt8k_codec = av1` puts `av1_nvenc -preset p5 -tune hq -multipass qres -tier 1 -highbitdepth 0` with the same VBR maths in front (logged `job: 8K step: yt8k_codec av1, yt8k_bitrate_mbps N -> av1_nvenc ... (fallback: hevc_nvenc ..., then libx265 crf 18)`); not listed / failed -> "falling back to HEVC" (no libaom / SVT-AV1 fallback: far slower than x265 at 8K); Matroska `stereo_mode` (`top_bottom` / `left_right`) written directly), MP4 (+ sidecar) delete only after verification, balloon notices. The graph is built by one helper (`BuildLayoutGraph(layout, shortSide, fpsCap)`; `shortSide` 0 = native size, else the frame's short side S) used by the MKV and 8K steps. The flat, MKV (re-encode) and 8K steps share one NVENC detection (`-encoders`, once per job: bit 0 hevc_nvenc, bit 1 av1_nvenc) and one runner (`encode(step, input, tries, swWarn, tail, out, ran)`) that tries an ordered `EncTry` list (name, args, NVENC bit or 0 = software, note) until one exits 0; an unlisted NVENC encoder is skipped with `job: X not available -- using Y`, a failing one logs `job: X failed (...) -- retrying with Y`. AV1 sources (a `codec = av1` recording; codec from ffprobe, once per job): the NVENC attempts get `-hwaccel d3d11va` before `-i` (frames come back to system memory, filters unchanged; ffmpeg's software AV1 decoder in the Gyan essentials build is libaom, ~27 fps at 8192x4348 vs ~65 fps hardware, output bit-identical), software attempts decode in software. **`eye_crop`** (`EyeCrop`, `16:9` default | `4:3` | `4:3pad` | `none`, `[post_processing]`, the window's "Eye crop" = `ID_EYECROP`, table-driven Combo; enum `Pad43` is appended, the combo lists it after 4:3): `BuildLayoutGraph(layout, crop, shortSide, fpsCap)` puts `crop=iw/2:min(ih,2*trunc(iw/2*D/N/2)):x:trunc((ih-oh)/4)*2` in front of each eye's scale (both eyes identical geometry, even sizes, no crop when the eye is already wider than N:D), so flat SBS eyes taller than wide (4096x4630) become 16:9 (4096x2304) or 4:3 before scaling; tb_half 8K then is 7680x4320 (16:9) / 5760x4320 (4:3), `none` = the old tall frame (4320x4884). `4:3pad` adds `pad=w=max(iw,2*trunc(ih*16/9/2+0.5)):h=ih:x=trunc((ow-iw)/4)*2:y=0:color=black` between the crop and the scale (eye units, so the scale expression is unchanged and sees the padded eye): 7680x4320 with the 4:3 picture 5760 wide in the middle; file tag `_4x3pad`. `sbs_full` + a crop is a re-encode (`reencode = !dome && (layout != sbs_full || crop != none)`). `pp::PredictLayout` mirrors the filter arithmetic and `logFrame` logs `input WxH = eyes ..; eye_crop X -> centre-cropped to cw x ch[, black bars left and right to pw x ch (picture vw wide at x vx of each sw-wide eye)]; <layout> frame fw x fh` in both steps (needs ffprobe). Config keys `post_mkv`, `keep_mp4`, `post_flat`, `post_yt8k`, `layout_3d`, `eye_crop`, `post_when`, `ffmpeg_path` (parsed in `src/config.cpp`, ignored by the layer). ffmpeg runs at below-normal priority with no window; exiting the tray kills it and deletes the partial output. Every child (ffmpeg / ffprobe) starts `CREATE_SUSPENDED`, is assigned to one job object created in `Start()` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, closed in `Stop()`) and is then resumed, so a crashed / killed tray (or a logoff, which the message-only window never hears about) takes its ffmpeg with it -- also a hold-suspended one; a failed `AssignProcessToJobObject` is logged once and the child runs as before. **Hold** (`post_when`): `Processor::SetHold(hold, reason)` -- while held the worker does not start the next queued job (`StartableLocked`), a job between two ffmpeg runs waits in `RunFfmpeg` before `CreateProcess`, and a running ffmpeg is suspended with `NtSuspendProcess` (ntdll via `GetProcAddress`) and resumed with `NtResumeProcess` when the hold clears; `Stop()` resumes a suspended child before `TerminateProcess`. Forced jobs (`Enqueue(path, true)`, "Convert last recording now"; forcing a queued or running job upgrades it) are picked ahead of held jobs and never suspended. `State` carries `held`, `paused`, `holdReason`, `userHeld` (count), and `path` / `forced` / `runningId` / `format` of the running job. `Queued()` returns a snapshot of the waiting jobs (`pp::QueuedJob {id, path, force, forceSeq, userHeld, ownFormat, fmt}`, queue order) and `Remove(id)` drops a waiting one (never the running one), both for the window's queue view. See the next row for per-job formats, user-held jobs and `queue.json`. The tray computes the hold every 500 ms tick and right before each `Enqueue` (`UpdateHold` in `tray/tray.cpp`): `manual` = always (only forced jobs ever start; recordings are still queued, so the window's queue view lists them and Start now forces one; switching to `manual` leaves already-queued jobs queued and held and suspends a running non-forced job like any hold); `now` = never; `session_end` = a live owner with `state != ECS_NONE` (the layer releases the block -- `ownerPid = 0`, `ECS_NONE` -- as soon as the game's last capture-capable session is destroyed: `Snapshot().capable` goes false and the control loop releases ownership, and `StopControlIfIdle` stops the control thread, which releases it too; no layer change was needed); `game_exit` = that, or a "producer" still alive: the last live `ownerPid` is cached with a process handle (`TrackOwner`; the layer zeroes `ownerPid` on release) and each non-forced queued recording registers it (`AddProducer`, a duplicated handle, so pid reuse cannot hold the queue); producers are dropped when they exit or when the queue is empty. `post_when` is cached in `g_postWhen`, reloaded with the input bindings when `config.ini` changes (`CheckConfigChanged`) or the window saves. Tooltip: "N recording(s) waiting -- converts when the game closes / when VR stops", "conversion paused: <file> -- resumes ..."; icon blue while waiting / paused only when no game is live (green / red / amber keep priority). Hold ON / OFF, suspend / resume and producer exits are logged once each |
| `tests/*` | See "Tests" |

## Data path

```
game thread, inside xrEndFrame (before forwarding):
  first XR_TYPE_COMPOSITION_LAYER_PROJECTION, views[0] = left, views[1] = right
  each view: subImage.swapchain -> tracked swapchain -> image = most recently RELEASED index
             (xrReleaseSwapchainImage releases the oldest acquired one), imageRect, imageArrayIndex, pose, fov
  record_overlays (frames with more than one layer only): CollectOverlays -> the quad / cylinder / equirect / cube layers
     AFTER that projection layer (and BEFORE it when it has source alpha), each with its released image, rect, slice,
     eyes, flags, colour scale / bias, pose located in the projection space
  Recorder::SubmitFrame(ts, L, R, overlays):
     free ring slot? (else drop, counted "ring full")
     overlays: shelf-pack their sub-images into the slot's overlay atlas of their format family (each atlas made by the
               worker at the first layer of that family)
     IFrameSource::CopyEyes: GPU-wait(free fence) + copy L -> (0,0), R -> (leftWidth,0) [+ overlay sub-images (MSAA: resolve first) -> atlases] + signal(ready fence)
       D3D11: CopySubresourceRegion x2 (+ ResolveSubresource for MSAA) on the game's immediate context, Flush
       D3D12: one command list on the game's queue: barriers RT->COPY_SOURCE, CopyTextureRegion x2, barriers back
  -> queue {slot, ready value, ts}
encoder worker thread (private D3D11 device on the game's adapter, by LUID):
  Wait(ready) -> [overlays per eye: (behind layers + projection over them) + one draw per layer into the ring slot] -> [linear formats: compute pass, sRGB encode]
  -> [re-projection pass (symmetric fov / smoothing) | dome pass]
  -> VideoProcessorBlt RGBA->NV12 (BT.709 limited)
  -> Signal(free) -> IMFTrackedSample(DXGI surface) -> IMFSinkWriter::WriteSample (hardware HEVC / H.264 / AV1 MFT)
audio thread: WASAPI loopback -> int16 48 kHz -> WriteSample (AAC)
stop: drain, Finalize, st3d patch (+ sv3d, <name>.json sidecar in dome mode; the sidecar also with record_for_youtube), summary line in the log
```

Nothing on the game thread waits on the CPU. A slot whose previous copy has not completed on the GPU (D3D12) is a
dropped frame ("gpu busy"), never a stall. The game device's state is never changed: D3D11 copies / Signal / Wait /
Flush do not touch pipeline state, and D3D12 uses its own command list and allocators.

Supported swapchain layouts (all exercised by the tests): two swapchains (ETS2 / ATS on VDXR: two
3072x3264 `B8G8R8A8_UNORM_SRGB` swapchains, see the DLAA injector's `CAPTURE_FINDINGS.md`), one texture array with
`imageArrayIndex` 0/1, one wide texture with two `imageRect`s, MSAA swapchains (resolved first). The first layout and every
change of it is logged (`frame layout (frame N): ...`).

### Formats and gamma

The ring uses the typed UNORM member of the swapchain's copy family (`B8G8R8A8_UNORM` for `B8G8R8A8_UNORM_SRGB` /
`_TYPELESS`, etc.), so the copy moves bytes unchanged. `*_SRGB` swapchains hold sRGB-encoded bytes = what the video wants
(video processor input `RGB_FULL_G22_NONE_P709`). Non-sRGB swapchains hold linear light (OpenXR treats them as linear);
those get a compute pre-pass that sRGB-encodes. Measured: the NVIDIA video processor accepts `RGB_FULL_G10` input but
passes values through without the curve (grey 128 stayed 128 instead of 188), hence the explicit pass.
`config.ini gamma = srgb | linear` overrides the decision. Output is tagged BT.709 primaries / transfer / matrix, limited range.

### Re-projection stage: symmetric fov and head-motion smoothing

One pass on the encoder device does both. It exists when `symmetric_fov = 1` (default; needs a usable view fov at
record start) or `head_smoothing > 0`. With both off there are no resources and no pass: the video processor reads the
ring (or the linear pre-pass output) exactly as before.

**Symmetric fov.** Headset views are asymmetric (outer angle larger). A flat SBS player maps both halves onto the same
rectangle, so a direction (a point at infinity) lands at different x in the two halves -> parallax at infinity, the
eyes do not fuse. Fix: both eyes get the same symmetric frustum and the same orientation (parallel cameras).
`ComputeSymmetricFov` (at `Recorder::Start`, from the views' fov / size at that frame, fixed for the recording):
`tanH = min over both eyes of min(|tan angleLeft|, |tan angleRight|)`, `tanV` likewise; per-eye output size
`round_even(2 tanH densityH) x round_even(2 tanV densityV)` with `density = max over eyes of size / (tanR - tanL)`
(source pixel density). The SBS output is `2 eyeW x eyeH` at the source density (logged as "per eye at the source
density"); see "Recorded size" below for how `max_width` / the codec limit shrink it.
Output tans `(-tanH, tanH, tanV, -tanV)`, times `output_fov_scale` only while smoothing (without smoothing the
intersection is always covered). Output orientation for both eyes = `O` (= `H` without smoothing), i.e.
`C_e = q_e^-1 * O` instead of the canted-eye-preserving `q_e^-1 * O * H^-1 * q_e`. Eye positions are untouched (the
stereo baseline). Logged at record start (`recorder: symmetric fov: source L[..] R[..] -> output ±..° x ±..° per eye,
WxH per eye`). Tested: `run_jitter.ps1` raw vs rawasym (far marker same x in both halves within 1 px vs ~250 px apart),
`head_smoothing_test` (intersection, sizes, rounding).

**Head-motion smoothing** (`head_smoothing > 0`):

- **Input.** The layer copies each `XrCompositionLayerProjectionView`'s `pose` and `fov` into `EyeView` (pose in the
  projection layer's space; same space for both views). `SubmitFrame` also gets `XrFrameEndInfo::displayTime`. The
  game thread only packs these numbers into the queue item.
- **Filter** (`HeadSmoother`, encoder worker thread, once per written frame). Head `H = slerp(qLeft, qRight, 0.5)`.
  Filter state `S <- slerp(S, H, a)` with `a = 1 - exp(-dt / tau)`, dt from displayTime, `tau = 0.05 s * 30^((s - 0.1) / 0.9)`.
  Follow: `a += (1 - a) * smoothstep((e - f) / f)` with `e = angle(S, H)`, `f = head_follow_deg` (full speed at 2f), and
  a catch-up level that ramps to 1 in 0.1 s while `e > f` and back to 0 in 0.3 s once `e < f/4`, blending `tau` towards
  `min(tau, 60 ms)` (without it a turn ends with a slow `tau` tail from `f` down). Reset to `H` at the first frame of a
  recording and after a gap > 0.5 s. Output head `O = S`, with roll removed if `roll_lock` (same forward vector, camera
  +X perpendicular to the space's +Y; LOCAL / STAGE are gravity-aligned).
- **Edge guard** (`edge_guard = 1`): if the output frustum of either eye is not inside its source frustum (exact test:
  both are convex cones, so the 4 corner rays decide), `O` is replaced by `slerp(O, H, b)` with the smallest `b`
  (14-step bisection) that is covered. Only the output is clamped; `S` keeps filtering. Why it exists: with
  `output_fov_scale = 0.9` the margin is ~3 degrees per side, and +-2 degree yaw + pitch + roll jitter at the corners
  exceeds that in ~30 % of frames (measured: `run_jitter.ps1` noguard case), so without it black corners flicker.
- **Per eye.** Smoothed eye orientation `O * H^-1 * q_e` (keeps each eye's offset from the head, e.g. canted
  displays). GPU matrix `C_e = q_e^-1 * O * H^-1 * q_e`: a ray of the smoothed (output) camera -> the actual eye camera.
- **GPU pass** (`Recorder::Impl::Reproject` -> `ReprojDraw`, `src/reproject.h`, shared with the live share; encoder device, inside the device-manager lock): one full-screen triangle
  over the whole W x H SBS target, the pixel shader picks the eye by x. For output pixel `uv` (0..1, v down) of an eye:
  `d = (lerp(k tanL, k tanR, u), lerp(k tanU, k tanD, v), -1)` with `k = output_fov_scale`; `a = C_e d`; behind
  (`a.z >= 0`) = black; `t = a.xy / -a.z`; source `s = ((t.x - tanL) / (tanR - tanL), (tanU - t.y) / (tanU - tanD))`;
  outside [0,1] = black, else a bilinear sample in that eye's half of the source (clamped half a texel inside the half,
  so the other eye never bleeds in). OpenXR conventions: right-handed, -Z forward, +X right, +Y up; `angleLeft` and
  `angleDown` are negative.
- **Colour.** Source SRV = the ring's typed UNORM (or float) format, target = a texture of the video processor's input
  format (`fmt`, or `R8G8B8A8_UNORM` after the linear pre-pass): no sRGB decode / encode anywhere, bilinear filtering
  works on the encoded values. The linear pre-pass runs first and the pass reads its output, so the colour path is the
  same as without smoothing; `run_selftests.ps1`'s `*_smooth` cases (identity pose, `output_fov_scale 1` = exact
  identity mapping) pass the same grey-step check (worst 1 code value).
- **Recorded size** (`max_width`, codec limit). `FitStageOutput` (`head_smoothing.h`, at `Recorder::Start`) scales the
  stage output (symmetric `2 eyeW x eyeH`, or the raw `W x H` layout with smoothing only) by ONE factor
  `f = min(1, maxW / width, maxH / height)` with `maxW = max_width` (0 = none) capped at the codec limit (8192 HEVC / AV1,
  4096 H.264) and `maxH` = the codec limit; eye widths floor to even (the frame is never wider than `max_width`), the
  height rounds to the nearest even. The pass then renders straight into that size (`vpW x vpH` = the encoder size,
  `outW == vpW`: the video processor only converts RGB -> NV12, no scaling) -- the shader works in normalized
  coordinates of each eye's destination rectangle (`dst`), so only the rectangles shrink; the edge guard, `headCorr` /
  head-locked overlays and the smoother are angle-based and unaffected; overlays are composited into the ring at
  source size before the pass. Minification: `misc.w = taps` (1 up to 1.2x; else `ceil(1 / 2f)` clamped to 2..4, so the in-game 12240 -> 8192 at 1.49x gets 2x2) --
  the shader evaluates the same mapping one output pixel right / down, and averages an n x n grid of bilinear taps over
  that footprint in the source (each tap ~2x2 texels), so a 4096-px eye shrunk to 640 px does not shimmer; taps = 1 is
  the old single-sample code path. Logged: `recorder: re-projection rendered at WxH (left L | right R; max_width N;
  native WxH, X.XXx smaller per axis, nxn bilinear taps per output pixel) -- the video processor does not scale` (or
  `... (native size; max_width N)`); the summary line's `re-projection WxH` is the rendered size. The dome keeps its own
  size (`dome_width`) and is still scaled by the video processor; with no stage (both off) the video processor scales
  as before. Mock 8192x4630 SBS (4096x4630 eyes, RTX 5090, `run_jitter`-style motion, 5 s): native 6768x4630
  0.087-0.091 ms GPU / +120 MB; `max_width 3840` 0.094 -> 0.061 ms, +120 -> +38 MB; 1280: 0.095 -> 0.082 ms (3x3 taps),
  +4 MB; smoothing + 2560: 0.093 -> 0.030 ms, +17 MB (before -> after; the mock scene is cheap to sample, so the GPU
  numbers are small either way -- the VRAM and the full-size video-processor scale that is gone are the larger part).
  Tested: `head_smoothing_test` (sizes, rounding, taps), `run_jitter.ps1` case `small` (max_width 1280).
- **Record for YouTube** (`record_for_youtube = 1`, `src/yt_layout.h`, `Recorder::Start`). The stage renders the two eyes
  already cropped, scaled and packed in the YouTube layout (`layout_3d`, `eye_crop`), the encoder writes that frame and the
  tray only remuxes it. Flat recordings only: with `projection != flat` it is ignored (one WARN). It needs the symmetric
  frustum: with `symmetric_fov = 0` the stage is turned on with symmetric fov for that recording (INFO
  `record_for_youtube: symmetric_fov = 0 overridden`); with no usable view fov at record start it logs a WARN and records the
  normal SBS way (no YouTube layout, no sidecar). Geometry: source eye = the symmetric frustum at the source density,
  `sym.eyeW x sym.eyeH`. Crop: `ch = 2 floor(eyeW D / N / 2)` if that is `< eyeH` (N:D = 16:9 / 4:3; none = no crop) --
  the eye keeps its full width and loses height top and bottom equally, i.e. the output vertical tan range becomes
  `tanV * ch / eyeH` (passed to the smoother as its symmetric output, so the edge guard tests the cropped frustum: a
  subset of the uncropped one, so it is covered whenever the uncropped one is). `4:3pad`: the 4:3 crop, then black bars
  left and right in eye units, `pw = max(cw, 2 floor(ch 16/9 / 2 + 0.5))` (rounded: the Quest 3's ch 3072 gives 5462 ->
  7680 wide; floored, 5460 would give 7678), `padX = 2 floor((pw - cw) / 4)`; from there the packed eye is `pw` wide (sw
  from pw), and the picture inside it is `vw = 2 floor(cw sc / wDiv + 0.5)` wide at `vx = 2 floor(padX sc / wDiv + 0.5)`
  (`YtLayout` / `pp::LayoutSize` `pw`, `vw`, `vx`; = cw / sw / 0 for the other crops). Frame: `PredictYtLayout(eyeW, ch, layout,
  crop, shortSide)` = a copy of the tray's `pp::PredictLayout` (identical results, checked by `head_smoothing_test` over a
  sweep), i.e. exactly what the 8K step would make from an SBS recording of `2 eyeW x eyeH`; `shortSide` = 4320
  (`XR_EYE_CAPTURE_TEST_YT_SHORT=<n>` overrides it for tests, logged). Then `FitYtLayout`: long side `<= VideoCodecMaxSide`
  (4096 H.264) and, with `max_width`, frame width `<= max_width` -- one uniform factor, each eye's `sw` / `sh` floored to
  even (`vw` / `vx` with the same factor, even, inside `sw`). That frame is the stage output and the encoder size (`vpW x vpH = outW x outH`; the video processor only
  converts to NV12). Destination rectangles: tb layouts left eye `(0, 0, sw, sh)`, right eye `(0, sh, sw, sh)`; sbs layouts
  left `(0, 0, sw, sh)`, right `(sw, 0, sw, sh)`; `swap_eyes` swaps the views before `SubmitFrame`, so eye 0 (top / left)
  is the right eye then. `4:3pad`: each destination rectangle is the picture (`x + vx`, width `vw`); the eye pick below
  still uses the packed areas, and the shader returns black for `uv` outside 0..1, so the bars are black. The pixel shader picks eye 1 when `pos.x >= misc.z && pos.y >= misc2.x` (eye 1's x0 / y0;
  side-by-side has y0 = 0, so the old `pos.x >= misc.z` pick is unchanged). Anti-aliasing: `YtTaps` = the
  `FitStageOutput` rule on the larger minification of the two axes (`eyeW / sw`, `ch / sh`); the footprint is per axis,
  so a one-axis squeeze is filtered on that axis and an upscale on the other is plain bilinear. Quest 3 (4096x4630
  symmetric eye): 16:9 -> crop 4096x2304 -> tb_half 7680x4320 (each eye 7680x2160, 1 tap); 4:3 -> 5760x4320; 4:3pad ->
  crop 4096x3072, pad 5462x3072 -> 7680x4320 (picture 5760 wide at x 960, 2x2 taps: `YtTaps` uses `vw`); none ->
  4320x4884; H.264 -> 4096x2304; `max_width 3840` -> 3840x2160. st3d: `stereo_mode` 1 (top-bottom) for tb layouts, 2
  (left-right) for sbs. Sidecar `<name>.json` (one line, see the dome section for its lifetime):
  `{"projection":"flat","layout":"top_bottom","packing":"tb_half","eye_crop":"16:9","eyes":2,"width":7680,"height":4320,"youtube":1}`
  (`layout` = `top_bottom` for tb_half / tb_full, `left_right` for sbs_half / sbs_full; `packing` = the `layout_3d` key;
  `eye_crop` = `16:9` / `4:3` / `4:3pad` / `none`;
  `width` / `height` = the encoded frame). Logged at init: `recorder: YouTube layout: eye 4096x4630 -> crop 16:9 4096x2304
  -> tb_half frame 7680x4320 (each eye 7680x2160; short side 4320, long side <= 8192), st3d top-bottom -- output
  ±a° x ±b° per eye` (with ` -> scaled to WxH (each eye WxH; max_width N | H.264 limit 4096)` before `, st3d` when a limit
  applies; 4:3pad adds ` -> black bars left and right to 5462x3072 (picture 5760 wide at x 960 of each eye)` after the crop,
  and `; picture 5760x2160 at x +960 in each, black bars left and right` after `... each` in the next line) and `recorder: re-projection rendered at 7680x4320 (YouTube tb_half: left eye at 0,0 | right eye at 0,2160,
  7680x2160 each; from 4096x2304 per eye, 1.88x wide, 0.94x tall; 1x1 bilinear taps per output pixel) -- the video
  processor does not scale`; the summary line says `layout YouTube tb_half (eye_crop 16:9, st3d top-bottom)` (`layout
  SBS` / `layout vr180 dome` otherwise). Overlays are composited into the ring before the stage as always, the in-headset
  indicator is not affected, head smoothing / edge guard / head-locked overlays are angle-based and work on the cropped
  range. The file name stays `<exe>_<stamp>_SBS.mp4`. Tested: `head_smoothing_test` (sizes, limits, taps, equality with
  the tray), `run_selftests.ps1` `*_yt_*` cases (below).
- **Cost.** One extra W x H texture (+318 MB at 12240 x 6496) and the pass: 0.20 ms avg / 0.74 ms max GPU at
  12240 x 6496 on an RTX 5090 (D3D11 timestamp queries, logged), 0.01 ms worker CPU. The game thread's `SubmitFrame`
  is unchanged apart from copying 2 x 8 floats.
- **Stats** in `re-projection summary (head smoothing on|off, symmetric fov on|off)`: mean / max correction (angle O vs H), frames where the edge guard limited it,
  frames with uncovered output (black), frames with follow / catch-up active, frames without a usable pose (identity
  pass-through), filter resets, GPU / CPU cost.
- **Translation** is ignored on purpose (needs depth). Head-locked projection spaces (VIEW) give identity poses:
  nothing to smooth.
- **Head-locked overlays** (`record_overlays`, VIEW-space layers such as the DLAA injector's panel): `HeadSmoother`
  also returns `SmoothFrame::headCorr = O * H^-1` (after the edge guard; identity without a pose) and the compositor
  moves those layers by it before drawing, so they stay fixed to the output camera instead of shaking with the residual
  jitter. See "Overlays", Compositing. Only with `head_smoothing > 0` (not in dome mode, not for `symmetric_fov` alone).

### VR180 / VR360 dome (`[dome] projection = vr180 | vr360`)

Replaces the flat re-projection: with `projection` != `flat` the stage always runs, `symmetric_fov` / `head_smoothing`
/ `head_follow_deg` / `roll_lock` / `output_fov_scale` / `edge_guard` are ignored (one INFO line), `swap_eyes` still
swaps the eye views before anything else. `projection = flat` (default) is untouched: no dome resources, no dome pass,
the same log lines plus `dome: projection flat` at load.

- **Conventions** (OpenXR, as in `head_smoothing.h`): right-handed, -Z forward, +X right, +Y up. Equirect pixel (u, v)
  in [0,1] of an eye (v down, pixel centres): `lon = (u - 0.5) * lonSpan` (pi for vr180, 2 pi for vr360; lon > 0 =
  right), `lat = (0.5 - v) * pi` (lat > 0 = up), dome ray `r = (cos lat sin lon, sin lat, -cos lat cos lon)`.
- **Dome frame D** (`ComputeDomeFrame`, worker thread): fixed at the first frame of the recording with a usable pose
  (same validity test as the smoothing input): `H = slerp(qL, qR, 0.5)`, forward `H * (0,0,-1)` projected on y = 0 and
  normalised, `D = Ry(atan2(-f.x, -f.z))`, i.e. yaw only (horizon level, gravity-aligned because LOCAL / STAGE are).
  Horizontal length < 0.05 (looking straight up / down): `D = identity` (-Z). Logged (`dome frame D fixed ... yaw`).
  A marker at infinity in world direction -Z therefore sits at `lon = +yaw(D)` (OpenXR yaw is positive to the LEFT,
  lon is positive to the right). D is fixed for the recording (`dome_forward = start`; `reset_on_hotkey` is parsed and
  logged as not implemented).
- **Per eye**: `M_e = q_e^-1 * D` (row-major, `a = M_e r` is the ray in that eye's camera). `a.z >= 0` or
  `t = a.xy / -a.z` outside the eye's fov tans = not covered, else a bilinear sample of that eye's half of the source
  (clamped half a texel inside, like the flat pass). Eye positions are not used: each eye's dome is centred on that eye
  (stereo baseline like a VR180 camera; a far point lands on the same equirect pixel in both eyes). Head jitter
  disappears by construction (world-locked), so there is no filter.
- **Coverage weight w** (`window_feather_deg` = f): 0 outside, else `saturate(asin(min_i dot(n, p_i)) / f)` with `n`
  the normalised camera ray and `p_i` the four normalised inward fov edge planes (`(1,0,tanL)`, `(-1,0,-tanR)`,
  `(0,-1,-tanU)`, `(0,1,tanD)`), i.e. the angular distance to the nearest window edge; f = 0: w = 1 inside.
- **Layout / size** (`ComputeDomeSize`, at `Recorder::Start` from the views of that frame): vr180 = per-eye square,
  SBS `2W x W`; vr360 = per-eye `W x W/2`, top-bottom `W x W` (left eye on top). `density = max over eyes of
  eyeWidth / (tanR - tanL)`, `W = round(density * lonSpan)` to even (vr360: to a multiple of 4 so each half has an
  even height for 4:2:0), or `dome_width`, clamped to an 8192 frame (vr180 W <= 4096, vr360 W <= 8192); no usable fov
  at the start frame: `density = eyeWidth / 2` (logged). The pass picks the eye by its destination rectangle
  (`pos >= eye[1].dst.xy`), which covers both layouts. Mock (1408 px per eye, -50/+40): 693.3 px/rad -> vr180 2178 per
  eye (frame 4356x2178), vr360 4356 (4356x4356). Quest-3-like 3072 px per eye with the same fov: 1512.7 px/rad ->
  vr180 wants 4752 -> 4096 (8192x4096), vr360 wants 9504 -> 8192 (8192x8192).
- **Fill = last_seen** (persistent dome on the encoder device): colour A/B (the stage format) and last-write time A/B
  (`R32_FLOAT`, seconds since the first recorded frame's displayTime) plus the pass output (`rpTex`, what the video
  processor reads). One full-screen pass with 3 render targets (output, new colour, new time), SRVs = eye source, old
  colour, old time: `dome' = lerp(dome, sample, w)`, `time' = w >= 0.5 ? now : time`,
  `fade = fill_fade_s == 0 ? 1 : 1 - saturate((now - time' - fill_fade_s) / 2)` (black 2 s after `fill_fade_s`),
  `output = dome' * fade`; then A and B swap roles. The dome is cleared at the first frame (black, time -1e9) inside
  the device lock. Frames without a usable pose paint nothing (the dome is shown as it is, fading). Colours stay in the
  encoded (sRGB) domain like the flat pass. VRAM: output + 2 x colour + 2 x time (logged: 181 MB at 4356x2178,
  ~640 MB at 8192x4096). **Fill = black**: one target, `output = sample * w`, no persistent textures.
- **Stats**: `re-projection summary (dome vr180, fill last_seen): N frames | dome: P frames painted, Q frames without
  pose | D yaw ... | re-projection WxH: GPU ...`. Measured with the mock (RTX 5090): 0.064 ms avg GPU at 4356x2178 with
  last_seen, 0.022 ms with fill black.
- **MP4 boxes** (`PatchStereoSpherical`, Spherical Video V2 RFC): children of the video sample entry, `st3d` first
  (FullBox v0 + `stereo_mode`: 2 = left-right for vr180, 1 = top-bottom for vr360), then `sv3d` (Box) { `svhd` (FullBox,
  `"OpenXR-EyeCapture\0"`), `proj` (Box) { `prhd` (FullBox, yaw / pitch / roll int32 16.16 = 0), `equi` (FullBox,
  uint32 0.32 bounds top, bottom, left, right = the fraction of the sphere NOT covered from each edge: vr180
  0 / 0 / 0x40000000 / 0x40000000, vr360 all 0) } }: 111 bytes, inserted with the same grow-the-ancestors patch as the
  flat 13-byte `st3d` (`PatchSt3d` is now a wrapper, flat output unchanged). ffprobe: `Spherical Mapping` /
  `tiled equirectangular` with `bound_left=2179`, `bound_right=2177` (ffmpeg converts the 0.32 fractions to pixels of
  the 2 x 4356 wide full sphere and rounds) for vr180, `equirectangular` for vr360, plus `Stereo 3D` `side by side` /
  `top and bottom`.
- **Sidecar** `<name>.json` (dome recordings and `record_for_youtube` recordings -- see "Record for YouTube" for that
  line -- only; no sidecar = flat SBS, so plain flat recordings are unchanged; the layer writes it only after a finalized
  file and deletes `<name>.json` together with the MP4 when a recording fails):
  `{"projection":"vr180","layout":"left_right","eyes":2,"width":4356,"height":2178,"dome_width":2178}` (encoded size;
  `dome_width` = encoded per-eye width). The tray reads it per job (status path and fallback scan both end in
  `RunJob`) and deletes it together with the MP4 when `keep_mp4 = 0`.
- **MKV vs MP4 route**: ffmpeg cannot set spherical metadata from its command line, but its mov demuxer turns `sv3d`
  into spherical side data and the Matroska muxer writes that as the `Projection` element (ProjectionType 1 =
  equirectangular, ProjectionPrivate = the equi bounds for the tiled case). Verified (`run_dome.ps1` e): after the
  tray's own `-c copy` remux the MKV still has `Spherical Mapping` (tiled) equirectangular with the same bounds, the
  Stereo 3D side data, the `stereo_mode` tag (`left_right` / `top_bottom`) and the raw `Projection` element
  (0x7670, ProjectionType 1). So the tray keeps ONE route for every recording: MKV, upload the MKV. YouTube's own
  handling of either file is not verified (no upload in the tests); the MP4 carries the same boxes as Google's
  spatial-media injector writes and is the fallback (`keep_mp4 = 1`).

### In-headset indicator

`Hook_EndFrame`, under `g_lock`, AFTER `CaptureSafe` (so the recording has already taken this frame's projection views):
`IndicatorTracker` maps the recorder state to what to show (starting / stopping = amber, recording = red pulsing
0.35..1.0 alpha at 1 Hz, idle after a recording: green 1.5 s if the message starts with `saved `, else red X 2 s;
`Failed` = red X 2 s; otherwise -- with `indicator_live = 1` (default) and a live viewer receiving frames
(`LiveShare::Active()`: sharing, a frame published within the last second) -- `IndShow::Live`, a steady violet dot;
otherwise hidden; the recorder states and the saved / failed flashes win over live). Hidden = no call at all. Visible: on first use `VrIndicator` creates, through
the NEXT layer's entry points (our hooks never see them, so they are not tracked as game swapchains), a 64x64 swapchain
(`R8G8B8A8_UNORM_SRGB` / `B8G8R8A8_UNORM_SRGB` if the game's colour swapchains are sRGB, else the UNORM ones; whatever
the runtime offers) and a VIEW reference space. Each shown frame: acquire / wait (100 ms) / `UpdateSubresource` of the
CPU picture on the game's immediate context / release, then an `XrCompositionLayerQuad` (BLEND_TEXTURE_SOURCE_ALPHA |
UNPREMULTIPLIED_ALPHA, straight alpha, both eyes, pose `indicator_x/y/z`, size `indicator_size`) is appended to a copy
of the layer pointer array (the game's pointers unchanged, both arrays on the hook's stack for the call). Skipped when
the game already uses `maxLayerCount` layers. Any failure (formats, create, acquire / wait / release, SEH fault) = log
once, destroy our objects, indicator off for the session. If `xrEndFrame` with our quad fails with a layer-type error,
the indicator is switched off and the game's original frame info is submitted again. `xrDestroySession` destroys our
swapchain / space before forwarding. Dormant sessions and sessions without a recorder never create anything.
D3D12 / Vulkan: not implemented (logged once at the first moment the dot would show). Settings snapshot per session.

### Overlays (`record_overlays`)

Quad (`XrCompositionLayerQuad`), cylinder (`XrCompositionLayerCylinderKHR`), equirect (`XrCompositionLayerEquirectKHR`
and `XrCompositionLayerEquirect2KHR`) and cube (`XrCompositionLayerCubeKHR`) layers that the game or another layer /
mod submits around the captured projection layer are composited into our copy of the eyes, the way a runtime shows
them. Default on; `[capture] record_overlays`, re-read per recording; the window's "Include overlay layers (menus, mod panels)".

- **Game thread** (`CollectOverlays` in `layer.cpp`, inside `CaptureImpl`, only while recording or while the live view
  composites overlays (`LiveShare::WantsOverlays`, "Live share (OBS)"; one collection serves both), and only when the
  frame has more than one layer): layers in `info->layers` order (back to front). Layers BEFORE the projection layer are
  collected only when the projection layer has `BLEND_TEXTURE_SOURCE_ALPHA` (`OverlayLayer::behind`, first in the list,
  `OverlayFrame::nBehind`; its `UNPREMULTIPLIED_ALPHA` flag goes along as `projUnpremultiplied`); with an opaque
  projection layer they are hidden by it and skipped with one log line each (`is BEFORE the projection layer, which is
  opaque`). Any other layer type (a second projection layer, `XR_FB` / `XR_HTC` / `XR_ANDROID` passthrough, unknown
  types) is skipped with a line naming the type and its `XrStructureType` value; only the base header is read. Per layer: the
  swapchain must be one created through our hooks (the same `SwapchainData` tracking as the projection swapchains:
  format / size / array / faces / samples from `xrCreateSwapchain`, images from `xrEnumerateSwapchainImages`, the most
  recently released index); not depth; a format of a known atlas family (`OverlayFamily`: R8G8B8A8 (UNORM + sRGB),
  B8G8R8A8, B8G8R8X8, R10G10B10A2, R16G16B16A16_FLOAT, R11G11B10_FLOAT); MSAA allowed (resolved on copy, see Atlas);
  `subImage.imageRect` clipped to the image, `imageArrayIndex` checked; cube: `faceCount` 6, square, not MSAA, the
  whole image, faces = array slices `imageArrayIndex * 6 + 0..5`; geometry checked (quad size, cylinder radius / angle /
  aspect, equirect radius >= 0 or +inf, finite scale / bias, equirect2 0 < central angle <= 2 pi, -pi/2 <= lower <
  upper <= pi/2); `eyeVisibility` -> an output-eye mask (swapped with `swap_eyes`); `layerFlags`:
  `BLEND_TEXTURE_SOURCE_ALPHA` (else opaque, texture alpha ignored) and `UNPREMULTIPLIED_ALPHA` (else premultiplied
  colour); an `XrCompositionLayerColorScaleBiasKHR` in the layer's `next` chain (at most 16 links walked) -> scale /
  bias (one on the projection layer is logged, not applied); the pose (taken from THIS frame's layer struct, never
  cached across frames; cube: its `orientation`, no position): a layer in the projection layer's own
  space is used as it is; a VIEW space created through our `xrCreateReferenceSpace` hook (tracked in `g_viewSpaces` with
  its `poseInReferenceSpace`, dropped at `xrDestroySpace` / session / instance end) is placed on the head of the
  SUBMITTED views (midpoint of the two eye positions, halfway rotation of the two eye orientations -- runtimes report the
  eyes at -+IPD/2 along the head's +X), so a head-locked panel stays at a fixed pixel even when the game submits poses
  that differ from the located head (ETS2 does; `run_e2e` stale-pose case: located placement would move it ~20-33 px)
  and is marked `OverlayLayer::headLocked` (head smoothing, see Compositing); any other space (LOCAL / STAGE / action
  spaces, or a VIEW space we did not see created) is located with
  `xrLocateSpace(layer space, projection space, frameEndInfo.displayTime)` through the NEXT layer's entry point,
  cached per space within the frame only, both VALID flags required. Layer pose =
  location * layer pose. At most `kMaxOverlays` = 16 per frame. Our own indicator is appended in `Hook_EndFrame` after
  the capture, so it is never in `info->layers` at this point. Every distinct (type, swapchain, space, behind, outcome)
  is logged once per session (`overlay: layer N (quad) RECORDED: ...` with format, MSAA, geometry, flags, colour
  scale / bias / `... not recorded: <why>`, at most 40 lines); `record_overlays = 0` logs one line when a frame first
  has other layers. Depth-test-free ordering = submission order (what the spec defines without a depth-test extension).
- **Atlas** (`Recorder::SubmitFrame` -> `OverlayPack`, game thread; the worker side is `OverlayAtlasSet` -- both in
  `overlay.h/.cpp`, shared with the live share's own ring): one shared texture per FORMAT FAMILY and ring slot,
  `kOverlayAtlasSize` = 4096 square, the family's TYPED UNORM / float member (`OverlayFamilyFormat`; typed because
  D3D11's `OpenSharedResource1` refuses a typeless D3D12 shared resource, E_INVALIDARG -- measured), created lazily: a
  layer of a family without an atlas only sets that family's bit in `OverlayAtlasSet::want`; the worker's next `Process`
  calls `OverlayAtlasSet::MakePending` -> `OverlayCompositor::Init` (once) + per wanted family
  `IFrameSource::CreateOverlayAtlas(..., family)`
  (D3D11: created on the encoder device with a shared NT handle, opened on the game device; D3D12: committed
  `HEAP_FLAG_SHARED` resources, `ALLOW_RENDER_TARGET`, opened on the encoder device -- the same scheme as the ring) and
  `OverlayCompositor::AddAtlas`, then publishes the bit in `ready`. Layers in between are recorded without
  (counted: "before their atlas existed"); a failed family is switched off for the recording (`failed`, counted).
  Sub-images are shelf-packed per family and frame (identical image / slice / rect once; a cube's 6 faces as one 3 x 2
  block of face-size cells, face f at column f % 3, row f / 3); one that does not fit is skipped (counted, logged once).
  `CopyEyes` copies them into the slot's atlases in the same submission as the eye copies, so the existing ready / free
  fences cover the atlases too (D3D11: `CopySubresourceRegion` on the immediate context, MSAA sources first
  `ResolveSubresource`d with the swapchain's typed format into a per-swapchain single-sample texture (`ResolveTarget`
  key = swapchain, index 2), the same slice resolved once per frame; D3D12: the overlay images RT -> COPY_SOURCE (MSAA:
  RESOLVE_SOURCE) -> RT, each used atlas COMMON -> COPY_DEST -> COMMON, a resolve target COMMON -> RESOLVE_DEST ->
  COPY_SOURCE -> COMMON, all in the same command list; MSAA overlays on a non-direct queue are skipped, logged once).
  The game's images are only read. VRAM per family in use: 64 MB per slot for 4-byte formats (192 MB for the 3-slot
  ring), 128 MB per slot for FP16, only once a layer of that family was seen.
- **Compositing** (`OverlayCompositor::Draw`, worker, inside the encoder device lock, right after the ready-fence wait,
  BEFORE the linear pre-pass / re-projection / dome pass, so overlays go through symmetric fov, head smoothing and the
  dome like the rest of the picture -- except that head smoothing does not shake head-locked layers, below), per
  output eye:
  1. Layers behind the projection layer (if any for this eye): the eye rectangle is copied to a "projection copy"
     texture, the eye is cleared to black (`ClearView`; what is below the first layer), the behind layers are drawn as
     below, then `psover` blends the saved projection OVER them with its own alpha (`P' + B (1 - Pa)`, `P' = P.rgb * Pa`
     when the projection is unpremultiplied, alpha written as 1; a B8G8R8X8 ring reads alpha 1). Where the
     projection is opaque the result is the projection again (decode / encode round trip).
  2. The layers after it, in order.
  Quad / cylinder (`psmain`): the mesh in the layer's local frame -- quad:
  4 vertices, size `size`, centred on the pose, front face +Z; cylinder: a strip of 2 * (n + 1) vertices, n = central
  angle / 2 degrees (even, 2..64), axis +Y, arc centred on -Z, height `radius * centralAngle / aspectRatio`, u = 0 at the
  left end -- is transformed to the projection space (layer pose) and into the eye camera with that eye's SUBMITTED pose
  (`E = q_e^-1 (P - p_e)`), then to clip space with the eye's asymmetric fov: `x = (2 E.x + (tanR + tanL) E.z) / (tanR -
  tanL)`, `y = (2 E.y + (tanU + tanD) E.z) / (tanU - tanD)`, `w = -E.z`, `z = w - 0.01` (near plane 1 cm; the hardware
  clips and interpolates uv perspective-correctly). Viewport = the eye's rectangle in the ring slot. Back faces culled
  (strip order bottom / top so the visible side -- the quad's front, the cylinder's inside -- is clockwise).
  Equirect / cube (`psray`): one full-eye rectangle; per pixel the ray `(tan, tan, -1)` of the eye's fov is rotated into
  the layer frame (`OverlayRayFor`: rows of `R = q_l^-1 q_e`, eye position `o = q_l^-1 (p_e - p_l)`). Equirect: finite
  radius r -> the far intersection with the sphere of radius r around the layer pose (`t = -b + sqrt(b^2 - c)`; no hit
  -> discard), its direction from the centre; radius 0 / +inf -> the ray direction. Azimuth `atan2(x, -z)` (0 = -Z,
  positive towards +X), elevation `asin(y)`; v1: `uv = (az / 2 pi + 0.5, 0.5 - el / pi) * scale + bias`, outside 0..1
  -> discard; v2: `|az| <= central / 2`, `lower <= el <= upper`, `uv = (0.5 + az / central, (upper - el) / (upper -
  lower))`, else discard. Cube: TextureCube rules on the layer-frame direction (major axis; face 0..5 = +X -X +Y -Y +Z
  -Z; `(s, t)` = (-z, -y), (z, -y), (x, z), (x, -z), (x, -y), (-x, -y); `uv = (s / |ma| + 1) / 2`), i.e. exactly what
  `TextureCube.Sample(direction)` does with the OpenXR (right-handed) direction -- the convention a runtime that
  samples its cube with the layer-space direction uses (not checked against a real runtime). `OverlayRayLookup` is the
  CPU mirror of `psray` (unit test). The destination copy for these is the whole eye.
  **Head-locked layers under head smoothing** (`headLocked`, i.e. VIEW space; `Draw(..., headCorr)`). The re-projection
  turns the picture from the actual head H to the smoothed head O. A VIEW-space layer is placed on the actual
  (submitted) head, so after the re-projection it moved with the residual jitter H vs O -- the DLAA injector's panel
  was "very shaky" in recordings with smoothing on, although world-locked content was steady. Now, per output eye e
  (position `p_e`) and per head-locked layer, `OverlayHeadLock` first rotates the layer about the eye by
  `R = headCorr = O * H^-1` (from `HeadSmoother::Update` of the same frame, computed before the device lock, after the
  edge guard): `q' = R q`, `p' = p_e + R (p - p_e)`, then draws it as usual into the actual eye. Why that is exact: the
  pass maps an output ray d to the actual-eye ray `a = C_e d`. Flat output: `C_e = q_e^-1 O H^-1 q_e`, so the output
  camera is the actual eye camera turned by R about the eye; a point drawn at p' is seen at output ray
  `C_e^-1 q_e^-1 (p' - p_e) = q_e^-1 (p - p_e)`, i.e. exactly where p is in the eye without smoothing. Symmetric output:
  `C_e = q_e^-1 O` and the output ray is `O^-1 (p' - p_e) = H^-1 (p - p_e)`, again the no-smoothing position (the
  parallel-camera mapping with `O = H`). So the layer keeps its pixel whatever O does (filter, follow / catch-up, roll
  lock, edge guard): a steady HUD on the smoothed camera, and with smoothing it lands where it would be without it. Eye
  offsets (IPD, canted eyes) are kept because the rotation is about each eye's own position; `swap_eyes` needs nothing
  (the item's eyes are already swapped, H is the symmetric halfway rotation). Cube / equirect layers in a VIEW space
  get the same rotation (the ray pass uses the moved pose). World-locked layers (the projection space, LOCAL / STAGE /
  action spaces located with `xrLocateSpace`) are not touched: they go through the re-projection like the scene and
  stay world-locked. Not applied: `head_smoothing = 0` (no correction at all; with `symmetric_fov` alone O = H, and
  the overlay path is byte-for-byte the previous one), dome mode (smoothing is off there; a head-locked panel stays
  where the head looks in the world-locked dome), frames without a usable pose (`noPose`), and a VIEW space that was
  not created through our hook (it is located with `xrLocateSpace` = a world-space pose and not recognised as
  head-locked). D3D11 and D3D12 games share this: the compositor runs on the encoder device for both.
  Blending is exact, in linear light: the ring is a typed UNORM texture holding sRGB-encoded bytes (or linear values for
  linear swapchains), so the destination rectangle under the layer (pixel bounding box of the projected vertices, the
  whole eye if a vertex is behind the near plane or for a ray layer) is first copied to a scratch texture (grown on
  demand, at most the ring size) and the pixel shader reads it, decodes sRGB, blends `c + d * (1 - a)` (`c = s.rgb * a`
  for unpremultiplied, `s.rgb` for premultiplied, `a = 1` without the blend flag) and encodes again, blending disabled.
  The source is sampled bilinearly by hand (4 `Load`s, texel coordinates clamped to the sub-image -- or the cube face --
  so atlas neighbours never bleed in), each texel sRGB-decoded first when the layer's swapchain format is `_SRGB` -- the
  same as filtering through an `_SRGB` view; then `s = s * colorScale + colorBias` (linear, before the alpha use).
  Stats: `recorder: first frame with overlays composited ...` and `overlay summary: N frames with overlays
  composited (M layer x eye draws) | layers handed over ... (K behind the projection layer): before their atlas
  existed, atlas full, atlas setup failed | atlases: <family bit mask>` after the recording summary.
- **Not done / limits**: Vulkan / OpenGL (the capture itself is a stub; logged), a second projection layer,
  passthrough layers (`XR_FB` / `XR_HTC` / `XR_ANDROID`; no image reaches the game -- skipped by name), colour scale /
  bias on the projection layer itself (logged), `XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT` and the chromatic
  aberration flag (ignored), other `next`-chain modifiers (depth info, FB settings / secure content, ...: ignored),
  R16G16B16A16_UNORM and other formats outside the six families (skipped, logged), MSAA cube swapchains, more than 16
  overlay layers per frame, the cube face orientation convention of real runtimes (the TextureCube rule above is what a
  straightforward runtime does; unverified). The headset's own UI (VD / Quest / SteamVR menus, guardian) is composited
  by the runtime / device after every OpenXR layer and cannot be seen by any layer.
- **Who sees the DLAA injector's quads**: the injector (`E:\ETS2-DLAA-Injector\src\xr_hook.cpp`) detours
  `GetProcAddress` and wraps the `xrGetInstanceProcAddr` of every OpenXR component at negotiation (API layers via
  `xrNegotiateLoaderApiLayerInterface`, the runtime via `xrNegotiateLoaderRuntimeInterface`); its quads (panel +
  fps box, VIEW space or the projection space, `BLEND_TEXTURE_SOURCE_ALPHA | UNPREMULTIPLIED_ALPHA`) are appended in
  the OUTERMOST `xrEndFrame` thunk (`t_depth == 0`), i.e. in front of the first API layer, and its swapchains are
  created / acquired / released through the outermost component's real functions -- our hooks. So our `xrEndFrame`
  receives them in `info->layers` and their swapchains are tracked. Seen in real logs: ETS2 / ATS sessions create the
  injector's `1824x1930` / `1824x2660` and `736x352` `R8G8B8A8_UNORM_SRGB` swapchains through our `xrCreateSwapchain`
  hook and log `layers this frame: projection quad` / `(2 game layers + 1, ...)`.

### Timestamps and A/V sync

Video: `XrFrameEndInfo::displayTime` deltas, anchored to QPC at the first recorded frame (re-anchored if the two clocks
drift more than 0.5 s). Audio: WASAPI's per-packet QPC position, kept continuous (gaps filled with silence, overlaps
trimmed). Both are relative to the first video frame. Frame rate written to the stream: the measured submit rate of the
game (EMA of displayTime deltas, snapped to 45/60/72/80/90/120/144), else `XR_FB_display_refresh_rate` if the app
enabled it, else `config.ini fps`; `max_fps` caps it. Timestamps are the real ones either way (variable frame rate is preserved) unless `constant_frame_rate = 1`.

**Constant frame rate** (`constant_frame_rate = 1`, `Recorder::Impl::Process` / `CfrFill` / `CfrIdleFill`): the video stream gets exactly one frame per 1 / fps (fps = that nominal rate). A frame's grid slot is `k = (rel * fps + 5000000) / 10000000` (rel = its time since t0 in 100 ns; integer maths, no drift); it is written at `k * 10000000 / fps` with the usual constant duration `10000000 / fps`. `k <= last slot` = dropped before any GPU work (counted, "dropped" in the CFR part of the summary; not a lost frame). `k > last slot + 1` = every missing slot repeats the last written frame: a pool surface is acquired, the NV12 surface that holds the previous frame is `CopyResource`d into it on the encoder device (under the DXGI device manager lock) and written at the slot's time -- before the current frame's video processor pass, which could otherwise reuse that very surface; when the pool hands out the surface that still holds the previous frame (the encoder released it, only the worker writes pool surfaces), it is written as it is. While no frame arrives, the worker wakes every 20 ms and repeats the last frame into the slots older than now - 250 ms (`CfrIdleFill`), so a stall is written as it happens instead of in one burst when the game resumes (a burst of hundreds of encodes held the worker and made the game thread drop the next frames: 75 ring-full drops after a 6 s stall at 3840x1080 with the arrival-only fill, 0 with the idle fill); frame times are predicted display times, ahead of the submission, so a frame still on its way does not land in an idle-filled slot. At most 5 s of repeats after the last game frame (logged once; the rest of a longer stall stays a gap, and the summary says "a gap over 5 s was only partly filled"). `max_fps` decimation in `SubmitFrame` stays as it is (CFR works on what arrives). Audio keeps its real timestamps; both streams are anchored at t0, so they stay in sync. Summary: `constant frame rate N fps, X repeated, Y dropped` (VFR: `variable frame rate (real timestamps)`).

### st3d

Media Foundation writes `moov` after `mdat`; the patch appends the 13-byte `st3d` box (dome: the 111-byte `st3d` + `sv3d` blob, see above) (stereo_mode 2 = left-right; 1 = top-bottom for `record_for_youtube` with a tb layout) to
the video sample entry (`hvc1` / `avc1`), grows every ancestor's size and rewrites only the moov region in place.
`moov`-before-`mdat` files are handled too (shrink a following `free` box, else a rewrite with every `stco`/`co64` +13)
but Media Foundation never produced one in the tests.

### Dormant mode (tray required)

`EyeCaptureTray.exe` (GUI mode only, not the `--start/--stop/--toggle/--status/--register/--unregister` CLI) holds the named mutex
`Local\OpenXR-EyeCapture-TrayAlive` (null DACL) for its whole life. At `xrCreateSession` the layer checks it
(`EyeCapTrayRunning()`: `OpenMutex(SYNCHRONIZE)`, access-denied counts as present). Missing and `require_tray=1`
(default; env override `XR_EYE_CAPTURE_REQUIRE_TRAY=0|1` beats `config.ini`) -> one INFO line "layer dormant", the session
is stored with `armed=false` and no frame source / recorder: `xrEndFrame` does nothing, swapchain hooks keep only the
session link (so dispatch stays correct) and skip all tracking / logging. The control thread (hotkey, status block,
command event) starts at the first armed session (`EnsureControl`) and stops when the last armed session is destroyed
(`StopControlIfIdle`; also on the last `xrDestroyInstance`). The check happens once per session: a tray started later does
not arm a running game; a tray closed later does not disarm it.

### Fault handling

All hooks forward the game's call and return its result unchanged. Bookkeeping failures are logged and ignored. The
capture work inside `xrEndFrame` runs under SEH (`__try` in `CaptureSafe`) plus `catch (...)` with `/EHa`, so a hardware
fault unwinds our locks, disables capture for that session (logged) and the game continues. The control thread pins
the DLL (`GetModuleHandleEx` + `FreeLibraryAndExitThread`), so an early unload cannot pull code from under it.

### Threads and locks

`g_lock` (layer.cpp) guards instance / session / swapchain maps; it is held during the capture work in xrEndFrame
(sub-millisecond). Lock order: `g_lock` → recorder internals. The recorder's worker and audio threads never take
`g_lock`. Session destroy waits (outside `g_lock`) up to 30 s for the finalize.

### Live share (OBS)

`obs-eyecapture.dll` (an OBS Studio source, `obs/`) shows the game's eyes live. The layer side (`src/live_share.cpp`) is
separate from the recorder: it works with or without a recording, and the recorder never sees it. `live_share = 1`
(default) allows it, `0` switches it off (read at session start). Side by side like the recorder's ring (left at x = 0,
right at x = left width). What the ring holds is the consumer's choice (`reproject`, the plugin property
"Re-projection"):

- **Same as recording (config.ini)** (`EYECAP_LIVE_REPROJ_CONFIG`, default): the recorder's flat re-projection
  ("Re-projection stage" above) with the same keys -- `symmetric_fov`, `head_smoothing`, `head_follow_deg`, `roll_lock`,
  `output_fov_scale`, `edge_guard` (`g_cfg.head`, so a recording-start reload is followed). Same rules as
  `Recorder::Start`: the symmetric frustum from the views' fov / size at the episode's first frame (fixed for the
  episode), the re-projection maps with smoothing and / or symmetric fov, both off = the raw eyes. With
  `record_overlays = 1` also the game's overlay layers, composited exactly like a recording ("Overlays" above; see
  "Overlays in the live view" below). No `max_width` (native size unless the viewer asks for Half), no dome
  (`projection` is ignored: the live view stays flat), no `record_for_youtube`.
- **Raw eyes (off)** (`EYECAP_LIVE_REPROJ_RAW`): the eye images as rendered -- no re-projection, no overlays.

And its size (`size`, the plugin property "Size"): **Full** (`EYECAP_LIVE_SIZE_FULL`, default) or **Half**
(`EYECAP_LIVE_SIZE_HALF`): the pass renders the exported ring at half width and height of the output eyes (`(n + 1) / 2`)
with the recorder's multi-tap minification (`misc.w` = 2: a 2 x 2 grid over each output pixel's footprint, i.e. an exact
2 x 2 box at the identity mapping, no aliasing). Raw eyes at Half go through the same pass with the identity mapping
(`ReprojIdentityMapping`: C = I, outTan = srcTan, so "raw" still means not re-projected). Half saves VRAM (the exported
ring is a quarter) and the consumer's copy (OBS copies a quarter of the bytes); it does NOT reduce the game-side cost:
the full-size `CopyEyes` on the game's GPU into the raw ring stays the same, plus the pass on the share device.

The in-headset indicator is never in the live view (it is appended after the capture, like for recordings).

**Discovery.** The CONSUMER creates the named block `Local\OpenXR-EyeCapture-Live` (`EyeCapLive`,
`src/live_share_proto.h`: magic `LIVE`, version 3) with an explicit security descriptor (SDDL
`D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)S:(ML;;NW;;;ME)`: full access for SYSTEM, administrators and logged-on users,
mandatory label Medium -- the default of an ELEVATED OBS, admins-only DACL + High label, made the normal game's
`OpenFileMappingW` fail with access denied, silently; the layer now logs that once per session: `live share: block ...
exists, but this game may not open it (access denied)`), and writes `consumerHeartbeat` (`GetTickCount64`) ~5x per second while
a source is showing (`obs_source_showing`), plus the fps it wants (`maxFps`, default 60), `reproject` and `size`. The layer, in an
ARMED session with a frame source, probes once per second from `xrEndFrame` with `OpenFileMappingW`: no block / heartbeat
older than 2 s -> nothing else happens (no thread, no device, no log line). A block held by another live producer (pid
alive, producer heartbeat fresh) is left alone; one producer per process (the first session that finds the consumer).
**Claim:** before it starts the worker, the producer takes `producerPid` with `InterlockedCompareExchange` (0, its own,
or a dead / stale producer's pid -> its own; losing = `took the block first -- not sharing this one`), then, as the only
writer, sets `slotCount = 0` under the seqlock (the consumer treats pid + slotCount 0 as "not live", so it never pairs
the new pid with an old producer's handle values) and starts its heartbeat, which the game thread keeps writing while
the worker sets up (a second game's probe sees a fresh producer). The setup re-checks ownership before it publishes;
in Starting / Live a producer that finds another pid stops (`another game took the block over`); `Publish` and the
teardown's unpublish write the seqlock part only while `producerPid` is still theirs. Teardown clears the pid (also
after a failed setup). Residual: only a seqlock write already in progress at the instant of a takeover (two games' claims
interleaved within microseconds, or a stale-but-alive producer resuming) can overlap the new owner's; the loser stops at
its next frame, and its setup / `Publish` re-check ownership before every seqlock write.
Elevation: an elevated OBS with a normal game works (the block's security above; the consumer's `OpenProcess
(PROCESS_DUP_HANDLE)` of a Medium process from High is allowed); an elevated game with a normal OBS does not (that
`OpenProcess` is denied, logged by the plugin).
Version 2 added `reproject` (consumer side, 8 bytes with its pad, so every producer field moved) and `reprojected`
(producer, `EYECAP_LIVE_OUT_SYMMETRIC | _SMOOTHED`, 0 = raw, in the former `pad0`). Version 3 put `size` in the consumer
pad (same layout) and added the `reprojected` bits `EYECAP_LIVE_OUT_HALF` (4) and `_OVERLAYS` (8); a block of another
version is ignored (logged once) and the plugin ignores a layer of another version (the magic / version check on both
sides), so plugin and layer must be updated together.

**Ring.** Consumer found: one log line (`live share: consumer found (pid N, wants N fps, full|half size) ... live view
re-projected like the recording: symmetric fov WxH per eye (...), head smoothing on|off` / `raw eyes (the viewer asked for
them)` / `raw eyes (symmetric_fov 0, head_smoothing 0: same as a recording)`; `no overlays (raw eyes)` / `no overlays
(record_overlays = 0)` / `overlays composited like a recording` / `overlays: none seen yet ...`; `no dome, never the
in-headset indicator`), then a worker thread creates a SECOND `IFrameSource` on the game's device / queue and a private
D3D11 "share device" (`CreateEncoderDevice(luid)`, the recorder's pattern).
- Raw eyes at full size (and "same as recording" with nothing to do: symmetric_fov 0, head_smoothing 0, no overlays):
  `CreateRing(shareDev, leftW + rightW, max(h), stageFmt, 4)` IS the exported ring.
- The pass (re-projection, overlays and / or Half): `CreateRing(..., 3)` is an INTERNAL raw ring (not exported); the
  exported ring is 4 textures of the output size (symmetric `2 eyeW x eyeH`, or the raw layout; each eye halved for Half)
  made on the share device (`RENDER_TARGET |
  SHADER_RESOURCE`, `SHARED_NTHANDLE | SHARED`) + two shared fences made there (`ID3D11Device5::CreateFence(SHARED)`), the
  `reproject.h` shaders, a constant buffer, a sampler, raw-slot SRVs and output RTVs. A format the share device cannot
  render / sample (`CheckFormatSupport`) or a failed setup falls back to raw eyes for that episode (WARN / ERROR).
Exports NT handles of the four ring textures (`IDXGIResource1::CreateSharedHandle`) and of the ready / free fences
(`ID3D11Fence::CreateSharedHandle`) from the share-device side (re-sharing an opened shared texture / fence works for both
the D3D11 ring and the D3D12 one), and publishes pid, generation + 1, handle values, size, format, sRGB flag, the eye
sizes (the OUTPUT eyes) and `reprojected` under the block's seqlock (`live share: ring ready -- generation N, 4 slots WxH
<fmt> re-projected (symmetric fov, head smoothing) + overlays, half size from a 3-slot raw ring WxH on the share device
(+N MB VRAM + the overlay atlases when the game shows overlays), T x T taps` / `... raw eyes, half size (identity pass)` /
`... raw eyes, handles exported`). The handles stay open in the layer for the ring's lifetime. The consumer `OpenProcess(PROCESS_DUP_HANDLE)` +
`DuplicateHandle`s them, `OpenSharedResource1` / `OpenSharedFence` on its own device (same adapter needed).

**Per frame (game thread, after the recorder's `SubmitFrame`, the same L / R views after `swap_eyes`, with their pose /
fov).** Producer heartbeat; consumer heartbeat stale -> stop. Eye size / format changed, the consumer's `reproject` or
`size` changed, or (re-projection asked) `g_cfg.head` / `record_overlays` changed, or the game shows its first overlay
layer while the ring has none (below) -> stop; a new ring (new generation) follows at the next probe, about a second
later. Throttle to `maxFps` on the frame's `displayTime` (the recorder's `max_fps` decimation).
- Raw eyes at full size: slot choice on the CPU only: not the latest slot, and not a slot whose value equals the consumer's `claim`
  while `free < value` (`GetCompletedValue` on the share-device fence); the slot's `slotValue` is set to 0 (interlocked =
  full barrier) and the claim re-checked, then `CopyEyes(slot, L, R, leftW, waitFree = 0)` -- two copies on the game's
  GPU, never a GPU wait for the consumer -- `slotValue = v`, and `(latestSlot, latestValue = v)` published. No usable
  slot = dropped frame (counted, "dropped busy"); a D3D12 slot whose previous copy is still on the GPU = "dropped gpu".
- The pass: a raw slot that is free, or whose last pass has completed (`ring.free >= ` the value the worker signalled
  after reading it; CPU check under a small mutex, no wait), else the slot of the frame still waiting in the mailbox (that
  frame is replaced, counted), else "dropped busy"; with overlays `OverlayPack` (the recorder's packing) into that raw
  slot's atlases; `CopyEyes(rawSlot, L, R, leftW, waitFree = 0, copies, nCopy)` (eyes + overlay sub-images in one
  submission); the item {raw slot, ready value, displayTime, both eyes' pose / position / fov / rectangle
  (`ReprojPoseUsable`), the overlay draws} goes into a ONE-item mailbox (a waiting item is replaced and its slot freed:
  never read) and the worker's auto-reset event is set. The game thread never writes the seqlock part in this mode.
- The time of each share call on the game thread is measured (QPC): the stop line reports the count, the maximum and the
  calls over 4 ms (`run_live.ps1` requires 0).

**Worker pass.** Waits on {stop, work}; takes the mailbox item (raw slot -> taken); picks an exported slot with the
raw-eyes rule above (none = "pass found no free slot", the raw slot freed); re-projection: `HeadSmoother::Update` (its OWN
instance: a recording at the same time keeps its own filter state; reset at each episode) -> `ReprojEyeMapping`;
otherwise (overlays only / Half only) `ReprojIdentityMapping`. `RpConstants`: src = the eye's rectangle in the raw slot,
dst = the output eye's rectangle (`(0 | outLeftW, 0, outEyeW, outEyeH)`: the symmetric frustum or the raw layout, halved
for Half), taps 2 for Half else 1. `Wait(raw ready, v)` on the share context; overlays: the atlases asked for
(`OverlayAtlasSet::MakePending`) and `OverlayCompositor::Draw` into the raw slot (head-locked layers follow the smoothed
head through `headCorr`, as in the recorder); `ReprojDraw` raw SRV -> output RTV, `Signal(out ready, ++n)`, `Signal(raw free,
v)`, `Flush`; raw slot -> done(v); then `slotValue = n` and `(latestSlot, latestValue = n)` published (the worker is the
seqlock writer in this mode). All of it on the share device's immediate context, which only this worker uses -- never on
the game's device or queue (beyond the copies) and never on OBS's device. Stats as the recorder's: `live share:
re-projection summary (head smoothing on|off, symmetric fov on|off): N frames | correction mean / max | edge guard limited
the output on N | uncovered (black) N | follow / catch-up N | no pose N | filter resets N`. Code shared with the recorder
(`src/reproject.h/.cpp`): `kReprojVs` / `kReprojPs`, `RpConstants`, `ReprojPoseUsable`, `ReprojSmoothIn`,
`ReprojEyeMapping`, `ReprojIdentityMapping`, `ReprojDraw`; with `src/overlay.h`: `OverlayPack`, `OverlayAtlasSet`,
`OverlayCompositor`.
Cost (mock, RTX 5090): 1408x1440 eyes -> 2 x 1164x1440 output, +98 MB VRAM (3 raw + 4 output slots); 1920x1080 eyes ->
+111 MB; game-thread share call max ~1-2 ms in the tests (the D3D11 `Flush` of the copies, as in raw mode). Real headset
sizes scale that: ETS2's 2 x 3072x3264 BGRA = 80 MB per raw slot, i.e. ~0.5 GB with the output ring -- unmeasured.
Half at 1920x1080 eyes: +63 MB (the 3 raw slots stay full size, the 4 output slots are a quarter). Overlays: + one 4096 x
4096 atlas per format family and raw slot = 192 MB per 8-bit family (384 MB for RGBA16F), the same as a recording's (the
live share and a recording at the same time each have their own).

**Overlays in the live view.** Only in "same as recording" with `record_overlays = 1`. The game thread collects the
overlay layers (`CollectOverlays`, once per frame for both the recorder and the live share) only while recording or while
`LiveShare::WantsOverlays()` (sharing in that mode). So a game without overlay layers pays nothing: the first ring of a
session has no overlays (the plain copy path when nothing else needs the pass); the first frame that shows an overlay
layer (seen by the live share's own collection, or earlier by a recording's) makes the session "has overlays" (sticky)
and, if the ring has none, stops it (`live share: stopped (the game shows overlay layers -- a new ring follows with them
composited)`), so the next ring (about a second later) is built with them. From then on: `OverlayPack` on the game thread
into the live share's OWN per-family atlases on its raw ring (`OverlayAtlasSet`, made by its worker on demand: `live
share: overlays: <family> atlas ...`), `OverlayCompositor::Draw` on the share device before the pass; stats `live share:
overlay summary: ...` at the stop (as the recorder's). Raw eyes never collect or draw overlays.

**Indicator (indicator_live).** `LiveShare::Active()` (sharing, a frame published within the last second) makes the
in-headset indicator show `IndShow::Live` -- a steady violet dot -- when nothing else is shown (starting / recording /
finalizing / saved / failed win). `indicator_live = 0` turns that off. Same D3D11-only limits as the indicator.

**Consumer (`obs/live_consumer.cpp`).** `Heartbeat(fps, reproject, size)`. Reads `(slot, v)`; skips it until `ready` has
completed `v` on the CPU (so the consumer's device never waits on the producer's GPU); `claim = (v << 4) | slot`, full
barrier, re-reads `slotValue[slot]` (changed = the producer is rewriting it: unclaim, next frame); `Wait(ready, v)` (already
satisfied), `CopySubresourceRegion` of the whole SBS slot into its own texture, `Signal(free, v)`, `Flush`. `free`
therefore only grows. Before it claims a NEWER frame, the previous copy must have completed on the consumer's GPU
(`free.GetCompletedValue() >= lastValue`, CPU check; else nothing this tick and the old claim stays): the producer's
reuse rule protects only the slot named by the current claim, so replacing the claim while the last `Signal(free)` is
still queued would let it rewrite the slot being copied (tearing). It cannot stall: a removed device reports
`UINT64_MAX`, and a new ring / a gone producer resets `lastValue` to 0. The claim / slotValue pair is a Dekker handshake (store, full barrier, load on both sides): either
the consumer sees the 0 and backs off, or the producer sees the claim and leaves the slot alone. A new generation,
producer pid 0 (torn down) or a producer heartbeat older than 2 s = drop the imports. The consumer side needed no change
for the re-projection beyond `reproject` / `size` / `reprojected`: the exported ring has the same protocol whichever thread
fills it.

**Teardown.** Consumer gone (heartbeat > 2 s old; `Close()` clears it so a clean exit stops the layer at its next
frame), eye size change, mode / size / settings change, first overlay layer, or session end: the game thread stops calling `CopyEyes`, then the worker
leaves its loop, unpublishes the ring (`producerPid = 0`), waits (<= 2 s) until its last pass has finished reading its
raw slot, `DestroyRing` (waits for the last copy), releases the small share-device resources (compositor, views, shaders, fences), closes the exported handles,
frees the big textures (overlay atlases, exported ring, raw ring) one per step with `ClearState` + `Flush` + 15 ms between
them (`kTeardownStepMs`; not paced at session end), then the share device, and
logs `live share: stopped (<why>) after N s -- frames shared, dropped busy, dropped gpu, skipped by max fps, replaced
before the pass, pass found no free slot | game thread: N share calls, max X ms, N over 4 ms`. Session destroy joins the
worker.

**Threads.** Game thread (`g_lock` held): probe, slot choice, `CopyEyes`, raw eyes: per-frame block writes;
re-projection: the mailbox. Worker: everything that could take time (device, rings, export, the re-projection pass and its
publishing, teardown); it never takes `g_lock`, and the game thread never waits for it except in `xrDestroySession`
(join) -- the mailbox mutex is held only for a few assignments on both sides. OBS: the plugin's `video_tick` (graphics
thread, enters the graphics context) does the heartbeat, producer check / import and the copy once per OBS frame (the
first showing source's tick; the others see the same `obs_get_video_frame_time` and return -- exact at any OBS fps, so
"Max fps" up to 240 is reachable at 120 / 144 fps OBS); `video_render` only draws.
All sources of one OBS process share one consumer (one heartbeat, one claim, one SBS texture, one `reproject`: the
re-projection when any showing source wants it, raw only when all of them want raw; one `size`: Full when any showing
source wants it) and each draws its own region (left / right eye with an optional 16:9 centre crop, or the whole SBS).
"Zoom" (100 .. 250 %, left / right eye views) is plugin-only: the drawn region is a centred crop of the eye by 100 / zoom
(then the 16:9 crop), drawn with `gs_draw_sprite_subregion` under a `gs_matrix_scale3f` to the source's un-zoomed size
(the source does not change size in the scene; bilinear upscale of the received pixels -- no detail is added).

**Colours.** The ring is typed UNORM (the copy family of the swapchain) with an sRGB flag (the recorder's decision, `gamma`
override applied). The re-projection samples and writes that same typed format (no sRGB decode / encode, like the
recorder's pass; a linear swapchain stays linear in the live view, the recorder's linear pre-pass is an encoder-only
step). The plugin samples sRGB-encoded 8-bit data through its texture's sRGB view (`gs_effect_set_texture_srgb`) and
linear data through the plain view, both with `gs_enable_framebuffer_srgb(true)`; sRGB-encoded 10-bit / FP16 (only with
`gamma = srgb` on such a swapchain: those formats have no sRGB view) goes through the plain view with the framebuffer
encoding OFF, so the encoded values pass through unchanged (before: sampled as linear and encoded again -> too bright).
The layer publishes the recorder's flag unchanged. Always the OPAQUE effect (eye alpha is ignored, as in the
recording).

## Tests (no headset needed)

| Test | Proves |
|---|---|
| `tests\run_selftests.ps1` | `encoder_selftest` over D3D11/D3D12 x two/array/wide/msaa, unorm (linear), h264, av1 (1080p D3D11 + D3D12, the 8K in-game size at 90 fps, and `*_noav1` = `XR_EYE_CAPTURE_TEST_NO_AV1=1`, which must fall back to HEVC and log `AV1 unavailable -- fell back to HEVC`), 3072x3264 per eye, and `*_raw_smooth_mw1280` (raw layout + head smoothing + `max_width 1280`: the scaled raw-layout re-projection, pixel-checked at 640x360 per eye, log `re-projection rendered at 1280x360`); the `out=` 8K cases assert `re-projection rendered at 8192x4348` with 2x2 taps (the video processor must not rescale); `*_yt_*` = `record_for_youtube` (`--youtube <layout_3d>`, `--eye-crop`, `XR_EYE_CAPTURE_TEST_YT_SHORT` = the frame's short side for the small cases): D3D11 two 1920x1080 sRGB HEVC `tb_half` short 540 -> 960x540 (each eye 960x270, no crop: 16:9 already), `sbs_half` short 540 -> 960x540 (each eye 480x540), 1200x1600 eye `--eye-crop 4:3` short 450 -> crop 1200x900 -> 600x452 (each eye 600x226), the same eye with `--eye-crop 4:3pad` -> padded to 1600x900 -> 800x452 (each eye 800x226, picture 600 wide at x 100, bars checked black), `tb_half --no-symmetric` (the override is logged) and the real size `tb_half` at 72 fps without the override -> 7680x4320 (4x upscale, 1x1 tap), all pixel-checked through `verify_mp4.ps1 -Packing`, each asserting the `YouTube layout: ...` line, `re-projection rendered at <frame>` with its taps, the summary's `layout YouTube <packing>` and the exact one-line sidecar: the frame-source + recorder + audio + st3d path the layer uses, with a game-like device created without video support. Each file checked by `verify_mp4.ps1`. Issue #1 cases: `*_cbr` (hevc / h264 / av1; the encoder must report rate control mode 0), `*_qp24` (hevc / h264: QP 24 reported back; av1: `constant quality 53 (from qp 24`), `*_qvs0` / `*_qvs100` (hevc / av1; reported back) and `*_cfr`, all with the normal pixel checks; then the QP size check: `encoder_selftest --detail --eye 960x540` (a noisy picture that changes every frame, 8 pre-made textures copied into the eye textures) at qp 20 and qp 45 for hevc / h264 / av1, the qp 45 file must be >= 4x smaller. |
| `tests\verify_mp4.ps1` | ffprobe: codec, size, fps, duration, `Stereo 3D / side by side`, BT.709 tags, raw `st3d` bytes after the `hvc1` / `avc1` / `av01` sample entry, AAC present / not silent; decoded pixels: grey steps within 6 code values in both eyes (gamma), left half = left eye. `-Packing tb_half|sbs_half|sbs_full|tb_full` (+ `-EyeCrop`, the frame as `-OutW` / `-OutH`): st3d `top and bottom` / raw stereo_mode 1 for tb, and the samples are mapped into each eye's packed rectangle (centre crop of the source eye, squeeze; the grey row = the middle of the part of band A the crop kept; `-EyeCrop 4:3pad`: inside the picture `vw` at `vx`, and the middle of each bar must be black). |
| `layer_loadtest.exe` | Real loader + active runtime (VDXR here): our layer is enumerated, negotiates, an instance is created and destroyed through it. |
| `tests\run_e2e.ps1` | Real loader → installed layer → `mock_runtime.dll`, a normal app frame loop for every layout and both D3D APIs, recording through `XR_EYE_CAPTURE_AUTORECORD`; each MP4 verified. Indicator: the mock validates every quad layer (known swapchain and space, rect inside the image, blend flag, unit quaternion) and prints the counts; D3D11 cases need a valid quad on >= the recorded frames and not on all frames, D3D12 none; an extra `vr_indicator = 0` case checks the recorded pixels where the dot would be (both eyes) equal the pattern and the indicator-on recording. |
| overlay cases of `run_e2e.ps1` | `layer_e2e --overlays` (D3D11 and D3D12, layout two): four layers after the projection layer, each from its own `R8G8B8A8_UNORM_SRGB` swapchain filled with a clear every frame (acquire / release like a game): A opaque green quad in the projection (LOCAL) space at (0, 1.45, -2), B 50 % white quad (`SOURCE_ALPHA | UNPREMULTIPLIED`) in a VIEW space at (-0.30, -0.128, -1), C orange cylinder in the VIEW space (radius 1.5, 0.4 rad, aspect 4) with texture alpha 0 and NO blend flag, D a magenta quad turned 180 degrees (back side to the viewer), E a cyan quad with `eyeVisibility = LEFT`, G a yellow projection-space quad with a new pose every frame (x = 0.4 sin(2 pi 0.1 tau); checked at video t = 1 s / 3 s against that frame's pose, within 4 px). Stale-pose cases (D3D11 / D3D12, `layer_e2e --stale-pose`, `symmetric_fov=0`): mock head motion, the app submits the views located 33 ms early; the VIEW-space cylinder's centroid must not move across 5 frames (measured spread 0.01 px). With `--overlays` the app submits the real eye positions (-+0.032, 1.6, 0) in the views. The mock runtime now implements `xrLocateSpace` (VIEW = its head pose) and counts cylinder layers. The frame at t = 2 s is decoded and the box at each layer's predicted pixel (head (0, 1.6, 0), eyes at x -+0.032, fov +-0.8 rad) checked in both eyes: A = green, B = the LINEAR-light blend over the pattern's eye-id band (the gamma-space blend, printed next to it, differs by ~40 in G / B), C = orange (alpha ignored), D = the pattern (culled), E = cyan in the left eye and the pattern in the right eye; D3D11: the indicator region still equals the pattern (the dot is never recorded). The wider layer set (same run; the projection layer has `BLEND_TEXTURE_SOURCE_ALPHA`, the pattern's band A is alpha 0; frame order 1 P, 2 K, 3 H, 4 projection, 5 A, 6 B, 7 C, 8 D, 9 E, 10 G, 11 I, 12 J, 13 M, 14 F, 15 T, 16 S; the mock now accepts 32 layers, creates cube swapchains as 6 slices per element (`TEXTURECUBE` on D3D11) and counts equirect / equirect2 / cube / other layer types): P an `XrCompositionLayerPassthroughFB` stub BEFORE the projection (must be skipped by name), K an opaque cube BEFORE the projection, turned +45 degrees about +Y, faces in distinct colours (-Z has a second colour for u < 0.25), seen only in band A where it adds to the premultiplied grey (4 points: two on the -Z face, one +X, one +Y; expected = sRGB(lin(grey) + lin(face colour)) from the TextureCube rules), H an opaque purple quad BEFORE the projection at (-1.864, 2.734, -2), 0.3 x 1.0 m: purple under the alpha-0 black grey step, red (hidden) under the opaque red block, I an infinite equirect2 (yaw -0.64 rad, 0.12 rad wide, elevation -0.39..-0.27) with two coloured halves (both halves + a point above it = the pattern), J an equirect v1 on a 10 m sphere around the head (scale / bias window, parallax of the eye offset in the expected pixel), M a 4x MSAA teal quad, F an R16G16B16A16_FLOAT quad (linear 0.2, 0.6, 0.2), T an R10G10B10A2_UNORM quad (linear 0.8, 0.3, 0.5), S a white quad with `XrCompositionLayerColorScaleBiasKHR` scale (0.5, 0.25, 0, 1) bias (0, 0, 0.2, 0) -> sRGB of linear (0.5, 0.25, 0.2) -- all within 12 in both eyes. Log lines `overlay: layer 5 (quad) RECORDED`, `layer 6 (quad)`, `layer 7 (cylinder)`, the passthrough skip line, `layer 2 (cube) RECORDED BEHIND`, `layer 3 (quad) RECORDED BEHIND`, `layer 11 (equirect2)`, `layer 12 (equirect)`, `4x MSAA (resolved)`, the FP16 / 10-bit formats, the colour scale / bias, `overlay summary: N frames with overlays composited`, `atlas setup failed 0`, the R8G8B8A8 / R16G16B16A16_FLOAT / R10G10B10A2 atlas lines, no MSAA / format skip. Extra D3D11 case with `record_overlays=0`: A / B / D show the pattern, C grey, H absent, band A plain grey, `record_overlays = 0` logged, no `RECORDED` / summary. The stale-pose cases also pass `--opaque-projection`: the cube and H must be skipped with `is BEFORE the projection layer, which is opaque` and no `RECORDED BEHIND`. `-OnlyOverlays` runs just these; `-BuildLayer` tests `build\Release\eye_capture.dll` without installing it (the registered implicit layer is disabled through `DISABLE_XR_APILAYER_JRDEVO_eye_capture`, the build copy enabled as an explicit layer with `XR_API_LAYER_PATH` / `XR_ENABLE_API_LAYERS`). |
| dormant case (last case of `run_e2e.ps1`) | Without `XR_EYE_CAPTURE_REQUIRE_TRAY=0` and without the tray: `layer_e2e --expect-dormant` loop completes, no MP4, log has the "layer dormant" line and no `control:` / recorder lines. (Fails if a tray is running.) |
| `overlay_test.exe` | `record_overlays` geometry on the CPU (the e2e setup): a quad's projected centre and top-left vertex land where the pinhole model puts them in both eyes, its front face is clockwise (drawn with `CullMode BACK`) and its back counter-clockwise (culled), the pixel box contains it and stays inside the eye; a cylinder seen from inside is clockwise and its arc centre lands where expected, seen from outside (convex side) counter-clockwise; a quad behind the eye gives w < 0 and the whole-eye box; zero size / no pose = nothing; the ray lookup of equirect / cube layers (`OverlayRayLookup`): equirect2 view centre -> image centre, right of centre -> u = 0.5 + azimuth / central angle (not mirrored), above -> v from the upper / lower angles, outside the angles / turned away -> not covered, v1 centre, v1 on a radius-2 sphere = the eye offset's parallax, +inf radius = infinite, scale / bias and uv > 1 -> not covered, cube centre -> -Z face centre, right / up -> smaller u / v on -Z (TextureCube rule), edges -> +X / +Y faces, turned 90 degrees -> +X; format families (atlas families: sRGB + UNORM 8-bit together, BGRA / 10-bit / FP16 / R11G11B10 separate). Measured on the e2e frame: A (0,254,0), B (231,189,190) vs linear 230,190,190 and gamma-space 228,148,148, C (254,126,0), D pattern, E (0,253,254) / pattern -- the same in D3D11 and D3D12. Head-locked quad under head smoothing (`HeadLockDrift`): `tests/test_motion.h` jitter + 30 degree turn (6 s at 90 Hz), mock fovs, smoothing 0.6, `output_fov_scale` 0.9, edge guard on, flat and symmetric output, eyes parallel and canted 2 degrees; the quad is placed on the submitted head like `CollectOverlays`, moved by `OverlayHeadLock` with the frame's `headCorr`, projected into the actual eye and taken through `C_e` to the output camera: its corner stays within 0.05 px (measured 0.0001 px) vs 22.8-37.5 px without the correction. (5 degree cant with symmetric output is never covered, the edge guard then pins O = H: no smoothing to test.) |
| `indicator_test.exe` | Indicator state machine (starting / recording / stopping / saved 1.5 s / failed 2 s / hidden) and the picture (colours, sRGB vs linear, RGBA / BGRA, pulse 0.35..1.0, opacity cap, anti-aliased edge, X vs disc, hidden = transparent). |
| `head_smoothing_test.exe` | Symmetric fov intersection, output size (density, even rounding), parallel-camera mapping without smoothing; filter math on the CPU: time-constant mapping, first frame = identity, correction direction for yaw / pitch (OpenXR conventions), gap reset, roll lock, follow + catch-up (lag bound, settle), edge guard on / off, no-pose pass-through. Dome: size derivation (vr180 / vr360, rounding, 8192 clamp, explicit `dome_width`, no-fov fallback), lon / lat -> ray (lon +90 = +X, lat +90 = +Y, lon 0 = -Z), D = yaw only (pitch / roll dropped, straight-up degenerate case, slerp of the eyes), eye matrix for a yawed head, a world direction maps to the same equirect pixel for any head pose. `record_for_youtube` (`yt_layout.h`): Quest 3 eye 4096x4630 -> 16:9 crop 4096x2304, tb_half 7680x4320 (eye 7680x2160), 4:3 5760x4320, none 4320x4884 (eye 4320x2442), sbs_half 7680x4320, sbs_full 8192x2304, tb_full 4320x4860; the selftest sizes; `PredictYtLayout` == the tray's `pp::PredictLayout` over a sweep (eye sizes x layouts x crops x short sides 0 / 4320 / 540 / 2160); limits: H.264 -> 4096x2304, `max_width 3840` -> 3840x2160, both, odd max_width; taps (1 for the Quest 8K frame, 2 for the selftest squeezes, clamp 4). |
| `tests\run_dome.ps1` | Dome end to end (installed layer, or `-BuildLayer` = `build\Release\eye_capture.dll` as in `run_e2e.ps1`; real loader, mock runtime `--jitter`: +-2 degree jitter + a 30 degree LEFT turn at t = 4..5 s, fovs L -50/+40 R -40/+50 x +-45), 11.5 s recordings `vr180` (last_seen, fade 8), `vr180_black`, `vr180_fade1` (`fill_fade_s 1`), `vr360`; Y plane decoded, marker located per eye, probe region at lon +33 / lat 0 (live before the turn, outside the window after it). (a) world lock: marker std-dev over t 1.0..3.9 s < 0.3 px, at lon = D yaw, lat 0 (2 deg). (b) mean after the turn = before (0.5 px); probe region still painted (last_seen) / black (fill black) at t ~ 5.5 s. (c) fade 1 s: painted at 5.5 s, black on every frame from 8.5 s. (d) right-eye marker = left-eye marker (mean <= 0.3 px, per-frame std-dev <= 0.15 px). (e) ffprobe on the MP4 and on the tray-command MKV: spherical (tiled) equirectangular + stereo side by side / top and bottom, the Matroska Projection element, the sidecar, the `_VR180` / `_VR360` names. Measured: (a) 0.058 / 0.085 px std-dev, lon -1.990 vs D yaw -1.99, lat -0.001; (b) 0.017 / -0.008 px; (d) mean 0.003 / -0.004 px, std-dev 0.09 px (per-frame max 0.32 px: each eye's own raster noise of the aliased test marker). |
| `tests\run_jitter.ps1` | Head smoothing end to end (installed layer, real loader, mock runtime): `layer_e2e --jitter` renders a world-locked grid + marker for the pose the mock reports (+-2 degree yaw / pitch / roll jitter at 3.3 / 4.1 / 4.7 Hz, a 30 degree yaw turn at t = 4..5 s; `tests/test_motion.h`). The mock's views have asymmetric fovs (left -50/+40, right -40/+50, +-45 degrees). Four recordings (smoothing 0 with `symmetric_fov` 1; smoothing 0 with `symmetric_fov` 0; 0.6 with `output_fov_scale` 0.9; same with `edge_guard` 0), ffmpeg-decoded, marker located per frame in both eyes. Checks: marker = analytic expectation (RMS < 0.15 degree), far marker at the same x in both halves with `symmetric_fov` 1 (<= 1 px) and not with 0, jitter-phase std-dev raw / smoothed >= 5, settles within 0.5 s after the turn (1 degree), final position 30 / 0 degrees, no black border pixels in the jitter phase. Measured: raw 2.00 degree, smoothed 0.31 degree (6.5x; 7.6x without the edge guard), settle 0.16 s (0.20 s without the guard), RMS 0.006 degree; far-marker x right - left: symmetric 0.29 px max (smoothed 0.50 px), raw asymmetric -244.5 px mean. hud cases (`hudraw` = raw, `hudsmooth` = smooth, both plus `layer_e2e --hud`: a green quad in a VIEW space at (0.12, -0.10, -1), 0.10 x 0.06 m, and a magenta quad in the LOCAL space at (-0.25, 1.75, -2); RGB decode, per-eye colour centroids): the VIEW-space quad stays within 1 px of its median over the whole recording in both eyes (it is absent in the first 3 frames: the overlay atlas is made at the first frame with an overlay), and the world quad's angle to the world marker has a std-dev <= 0.05 degree in the jitter phase (it moves with the world). Measured (`-BuildLayer`): VIEW quad max deviation 0.11-0.14 px (hudraw) / 0.62-0.78 px (hudsmooth; the aliased quad edges re-sampled at sub-pixel offsets), world quad 0.018-0.025 degree while its pixel position moves 17.6 px (hudraw) / 3.2 px (hudsmooth) std-dev; the installed pre-change layer (no `OverlayHeadLock`): VIEW quad max deviation 35.0-35.7 px in hudsmooth (the bug). `-BuildLayer` (as in `run_e2e.ps1`) tests `build\Release\eye_capture.dll`; `-OnlyHud` runs just the hud cases. Constant frame rate (`-OnlyCfr` runs only these): `cfr` = the raw case with `constant_frame_rate=1` through the layer (constant packet durations, `r_frame_rate == avg_frame_rate`, frame count = recorded time x fps +- 1, the summary line, marker RMS); `cfrjit` = `encoder_selftest --ts-jitter` (deterministic: up to +-35 % of a period off, every 29th frame missing, every 31st followed by a second one in the same period) as VFR and with `--cfr` (CFR: constant durations, r == avg, round(s x fps) +- 1 frames, repeated > 0 and dropped > 0, stream duration within 1 % of the VFR one; the VFR file is unchanged); `cfrgap` = `--cfr --ts-gap 6`: 450 repeats, the cap logged, one 1.011 s gap left, ring full 0. |
| `tests\run_settings.ps1` | app window, no GPU, no game: (1) `settings_test hotkeys` -- every key name the window can produce (243 VKs = all but Escape and the modifier / Windows keys, x 8 modifier sets = 1944 texts) parses back through `ParseHotkey` to the same key + modifiers + text; `MouseX1`, `MediaPlayPause`, `Key0xE5`, `Pad:A`, `Pad:Back+Start`, `Pad2:LB+RB+Guide`, `Joy:*:Button12`, `Joy:Thrustmaster T300RS Racing Wheel:Button5+Button6+POVUp` and the list `ScrollLock, Pad:Back+Start, Joy:*:Button1` round-trip; `Pad:Banana`, `Joy:x`, `Joy:x:Button0` / `129`, `Pad5:A`, `F7, ` ... are refused; (2) `settings_test save` -- the Save code path on a scratch copy of `config.ini`: `output_fov_scale = 7` and hotkey `Ctrl+Banana` refused (file byte-identical), unchanged values write nothing, bitrate / projection / hotkey changed in place (the before / after diff is exactly those 3 lines, comments intact), a key of a missing section is appended (CRLF kept); (3) `settings_test drive <pid>` -- a scratch tray (`build\tt2`, `EYECAP_TRAY_TEST_SHM`, scratch `APPDATA`) gets `WM_APP+3`, the real window is driven with `SendMessage` (Capture... captures F7, Add... + a posted `WM_XBUTTONDOWN` gives `F7, MouseX1`, Capture... + F7 again replaces it with `F7`, bad value refused and its message logged as `tray: settings message box [...]`, good values saved, the "Output layout" drop-down (Export group, found by `ID_LAYOUT3D`: same column as YouTube output, exactly one row above Eye crop) is enabled for flat projection, greyed (with its label) for vr180 and enabled again after switching back to flat, item 1 saves `layout_3d = sbs_half`; "Dome fill" / "Fade after" (combo, edit, labels) are enabled for vr180 / vr360 and greyed for flat; "Record directly in the YouTube layout (no re-encode)" (`ID_RECORDYT`) is greyed for vr180, loads `record_for_youtube = 0`, sits below Eye crop, greys the 8K bitrate / codec and their labels when ticked (enabled again when cleared) and is saved ticked: `record_for_youtube 0->1` in place (`run_settings.ps1` puts a `record_for_youtube = 0` line into the scratch `[capture]` when `config.ini` has none, so the final diff has 15 changed lines; `settings_test save` also checks the key: missing + 0 leaves the file alone, 1 is appended to `[capture]`, 0 is written in place); untouched head_smoothing slider keeps the file's `0.57`); `settings_test save` also round-trips all four `layout_3d` values and checks an unknown one is reported and keeps `tb_half`, and the same for `post_when` (`now` / `session_end` / `game_exit` / `manual`, default `game_exit`, case-insensitive, `later` reported; a missing key is appended to `[post_processing]` only when not the default); the drive step checks the "Convert" drop-down has 4 items (item 3 "When I say"), loads `game_exit` ("When the game exits") and saves item 0 as `post_when = now`, which the tray logs as `tray: post_when = now` without a restart; after the Resolution reopen rounds it reopens the window twice more: "Immediately" shown -> pick "When I say" -> Save (`post_when now->manual`, `tray: post_when = manual`, file ends with `post_when = manual`), then "When I say" shown -> Cancel; it also checks the "Convert a recording..." and "Add to queue..." buttons (visible, enabled; not clicked: they open a file dialog), the Conversions group of an idle tray (status "Nothing is converting.", progress 0, list "(no recordings waiting)" and multi-select, buttons "Start" / "Start all", Pause / Cancel / Start / Start all / Remove greyed), that the window height is <= 1032, and that every `settings: text fit check` line of the scratch tray says `N controls, 0 cut` (N >= 10) with no `text does not fit` line; the scratch config pins `eye_crop = 16:9`, `bitrate_mbps = 0`, `yt8k_bitrate_mbps = 150`, `codec = hevc`, `yt8k_codec = hevc` (the `save` scratch also pins `bitrate_mbps = 0`, so a live config with another bitrate does not break the 3-line diff); the drive step picks Codec AV1 (3 items; Default hint "about 36 GB per hour (AV1: 80 Mbit/s)") and 8K codec AV1 (greyed without 8K, "HEVC (default)" loaded), both shown again on reopen; `settings_test save` round-trips `yt8k_codec` (default hevc untouched when missing, av1 / hevc in place, `AV1` / `H265` case-insensitive, `vp9` reported) and `codec = av1` / `h264` (in place, `vp9` reported); the drive step checks the Bitrate drop-down (5 items, Default shown, hint "about 54 GB per hour", picks 250 -> "about 113 GB per hour"), the free-space line ("<drive>: .. free", follows the folder field), and the 8K bitrate drop-down (greyed for "3D MKV", enabled with 8K, 150 shown, picks 200); the reopened window shows 250 / 200; the expected changed-keys line has `bitrate_mbps 0->250`, `yt8k_bitrate_mbps 150->200`, `eye_crop 16:9->4:3` (14 changed lines with `codec hevc->av1` and `yt8k_codec hevc->av1`); `settings_test save` round-trips `yt8k_bitrate_mbps` 100 / 200 / 180 / 150 and reports 401 / 10; `FindDialog` only accepts the VISIBLE window (it exists, titled, while its controls are still being created and loaded -- the old check raced with that); then `tray.log` must show the `settings saved ... changed ...` line, `tooltip now: settings saved -- applies to the next recording` and `tooltip restored` 5 s later. `record_overlays`: `settings_test save` checks default 1, a missing key + default leaves the file alone, 0 is appended to `[capture]` and read back, 1 again is changed in place, `off` reads as 0, `maybe` is reported and keeps 1; the drive step expects the "Include overlay layers (menus, mod panels)" check loaded checked, unticks it, and `tray.log` / the diff must show `record_overlays 1->0`. `max_width` ("Resolution", `ID_MAXWIDTH`, items Native 0 / 3840 px (4K) / 2560 px (2K) / 1920 px (1080p); a file value that is none of them gets an extra "Custom (N px)" item -- `Field::custom`, the value is kept on Save unless another item is picked): `settings_test save` round-trips 3840 / 2560 / 1920 / 3000 / 0 in place in `[capture]` and checks 100 is reported; the drive step loads `max_width = 3000` as "Custom (3000 px)", saves it untouched, then reopens the window three times (Custom kept -> pick 4K; 4K shown, no Custom item -> pick Native; Native shown -> Cancel) and `tray.log` must show `max_width 3000->3840` and `3840->0` (15 changed lines in the final diff). Only the scratch tray is stopped. `settings_test save` block 14: the issue #1 keys (`beeps`, `rate_control`, `qp`, `quality_vs_speed`, `constant_frame_rate`): defaults, missing + default leaves the file alone, appended to `[capture]` and read back, `auto` / `cqp` / `024` count as unchanged, six bad values reported with the defaults kept. The window keeps both columns at 16 rows (two settings share a row: Audio + Device, Codec + Encoder preset, Frame rate limit + Constant frame rate, Head smoothing + FOV scale), so it still fits 1080p. |
| `tests\run_install.ps1` | Layer registration, no GPU, no game, no admin (refuses to run elevated); everything on test keys under `HKCU\Software\OpenXR-EyeCapture-Test` (deleted at start and end). (1) `install_test.exe`: NotRegistered -> Register -> exactly one value `<dir>\json` = DWORD 0 (another vendor's value and a look-alike `...json.bak` untouched), Register again = no change; a stale same-name value in another folder -> `StalePath` -> Register removes it; ours + a stale one with `/` and other case -> `StalePath` -> cleaned; ours = 1 (disabled) -> `NotRegistered` -> Register sets 0; ours in lower case -> `Registered`, Register rewrites the exact spelling; a CU copy -> `DuplicateHkcu` / `hkcuDuplicate`, `RemoveHkcuDuplicates` removes only it; Unregister removes every same-name value; missing json / dll -> `filesPresent` false, Register refuses with `ERROR_FILE_NOT_FOUND`; missing root key; a read-only key (protected DACL, read for Everyone = what a non-admin sees on HKLM) -> Register / Unregister `ERROR_ACCESS_DENIED` + "needs administrator rights", and an already-correct registration on it succeeds without writing. (2) a scratch `EyeCaptureTray.exe` in `build\tt3` with `EYECAP_TRAY_TEST_REG_ROOTS`: `--status` `registered=no` / `stale` / `yes` / `yes hkcu_duplicate=yes` / `files=missing`, `--register` exit 0 + value check, `--register` removes the CU copy, `--unregister` exit 0, no dll -> exit 1, read-only key -> exit 5, `tray.log` lines. (3) the same scratch tray as a GUI in test mode (`EYECAP_TRAY_TEST_SHM` + `EYECAP_TRAY_TEST_REG=1`): `tray.log` must show `registration: not registered`, the start question text answered No (test mode), `tooltip now: Not registered for VR games ...`; with a stale value the "registered from another folder" question; an outside `--register` is picked up by the 10 s re-query (`registration: registered (...)`, tooltip back to `no OpenXR game running`). (4) `build\Release\EyeCaptureTray.exe --status` on the REAL keys prints `registered=stale` (the repo root is the registered folder) and the real HKLM / HKCU OpenXR values are byte-for-byte unchanged. NOT covered: the real UAC / `runas` path and a real HKLM write. |
| `tests\run_reload.ps1` | Config reload at recording start (needs the installed layer; GPU-light, mock runtime; skips when `amtrucks` / `eurotrucks2` / `obs64` runs): scratch `APPDATA` config `projection = flat`, `bitrate_mbps = 20`; one `layer_e2e` process records 3 s, then (test-only `XR_EYE_CAPTURE_AUTORECORD2=3`) a second 3 s recording 1 s later; the script edits the config to `vr180` / `60` as soon as the first MP4 appears. Needs: `_SBS.mp4` then `_VR180.mp4`, log lines `config reloaded ...: no changes` then `...: changed bitrate_mbps=20->60, projection=flat->vr180`, recorder init lines `HEVC 20 Mb/s` then `HEVC 60 Mb/s`. (The MP4 bit rates themselves are only printed: the synthetic pattern compresses to ~1 Mb/s whatever the cap.) The second recording also flips `constant_frame_rate` 0 -> 1 and `beeps` 1 -> 0: the reload line must read `changed bitrate_mbps=20->60, constant_frame_rate=0->1, beeps=1->0, projection=flat->vr180`, `control: beeps off (config reload)` is logged and only the second summary says `constant frame rate`. `-BuildLayer` tests `build\Release\eye_capture.dll` (as in `run_e2e.ps1`). |
| `tests\run_yt8k.ps1` | The tray's 8K step (`post_yt8k`) and the `layout_3d` handling of the MKV step (`post_mkv`) end to end, CPU only (`EYECAP_TRAY_TEST_NO_NVENC=1` -> libx265, `EYECAP_TRAY_TEST_YT8K_SHORT=<n>` -> short side n instead of 4320): a scratch tray (`build\tt4`, `EYECAP_TRAY_TEST_SHM`, scratch `APPDATA` config with `ffmpeg_path`) converts synthetic recordings that `status_sim` announces. (A) 1280x360 SBS (640x360 eyes, testsrc2 left, solid blue right), 30 fps, 2 s + AAC, short side 432, `post_mkv 1` + `post_yt8k 1`, `keep_mp4 0`: `.mkv` + `_YT8K.mkv` made, MP4 deleted; `_YT8K.mkv` hevc 768x432 yuv420p 30/1, `stereo_mode=top_bottom`, ~2 s, audio copied; top half average colour = left eye, bottom half = right eye (within 10), no black bars at the edges; the logged command has the per-eye expression graph; balloon "Ready for YouTube: ..." + "YouTube 8K: ...". (B) 960x720 SBS (TALL 480x720 eyes), 72 fps, short side 480, `post_mkv 0`: only `_YT8K.mkv`, 480x720, 60/1 with 120 frames, log "capped to 60 fps" (`fps=60` in front of the graph), top = left eye, bottom = right eye, balloon "Ready for YouTube (8K): ...". (D / E / F / G) the case-A source (copies, short side 432, `post_mkv 0` + `post_yt8k 1`, `keep_mp4 1`, config rewritten between jobs because the tray re-reads it per job) with `layout_3d` = `sbs_half` (768x432, `left_right`, left eye in the left half), `sbs_full` (1536x432, `left_right`), `tb_full` (432x488: two 432x244 eyes, `top_bottom`, left eye on top) and `bogus` (768x432 `tb_half` + "bad value 'bogus' for 'layout_3d'" logged); each checks the tag, which half holds which eye's colour, and the logged layout line + `-vf`. (H / I, MKV step on copies of the case-A source, `post_mkv 1` + `post_yt8k 0` + `keep_mp4 0`, no short-side override) `layout_3d = tb_half`: `.mkv` is a re-encode, hevc 640x360 (one eye's size), 30/1, `top_bottom`, top = left eye colour, bottom = blue, audio copied, MP4 deleted after the logged `MKV verification (re-encode): exists, N bytes > 0, duration .. vs MP4 .. (within ..) -- ok`, no `fps=` in the graph; `layout_3d = sbs_full`: plain remux (`-c copy`, h264 1280x360, `left_right`, logged `MKV verification (remux): ... > 90%`). The 8K bitrate line is checked in (A) (default 150: `-b:v 150M -maxrate 200M -bufsize 300M`) and (B) (`yt8k_bitrate_mbps = 100`: `100M / 133M / 200M`). (C) `_VR180.mp4` + vr180 sidecar: "8K step skipped: VR180 ..." logged, nothing made, balloon "Nothing to convert". (Q) `yt8k_codec = av1` on the NO_NVENC tray: the logged av1_nvenc plan (VBR maths of `yt8k_bitrate_mbps = 100`), "av1_nvenc not available -- using hevc_nvenc (yt8k_codec av1: falling back to HEVC)", then libx265; output hevc + `top_bottom`. (S) an AV1 source (libaom-av1; skipped when the ffmpeg has none): the `sbs_full` remux keeps av1 + `left_right`, the 8K copy is hevc, "source video is AV1" logged, the libx265 command has no `-hwaccel`. (R) a second test tray WITHOUT `EYECAP_TRAY_TEST_NO_NVENC` and `ffmpeg_path` = `fake_ffmpeg.exe` (`FAKE_FFMPEG_ENCODERS=hevc_nvenc,av1_nvenc`): R1 runs `8K cmd (av1_nvenc)` with the AV1 options and no HEVC command and the file is `yr1_SBS_YT8K_full_AV1.mkv`; R2 (`FAKE_FFMPEG_FAIL_ON=av1_nvenc`) logs "av1_nvenc failed (...) -- retrying with hevc_nvenc (yt8k_codec av1: falling back to HEVC)", finishes with hevc_nvenc and renames `_YT8K_full_AV1.mkv` to `_YT8K_full.mkv` (logged); Q (libx265 fallback) is renamed the same way. (Y) a record_for_youtube recording: `yy_TB.mp4` (two 768x216 eyes stacked = 768x432, top testsrc2, bottom solid blue, 30 fps, 2 s, AAC) + its sidecar (`"projection":"flat","layout":"top_bottom","packing":"tb_half","eye_crop":"16:9",...,"youtube":1`), post_mkv 1 + post_yt8k 1 + post_flat 1, keep_mp4 0, `layout_3d sbs_half` / `eye_crop 4:3` (must be ignored): `yy_TB.mkv` is a stream copy (h264 768x432 like the MP4, `stereo_mode=top_bottom`, audio aac, no `_4x3` tag), logged "recorded in the YouTube layout (tb_half, 16:9, 768x432): remux only" and "already in the YouTube 8K layout -- 8K step skipped" (no 8K command, no `_YT8K` file), the MP4 and sidecar deleted after the remux verification line, `yy_TB_FLAT.mp4` 3840x2160 with square pixels and the top eye's colours (vf starts `crop=iw:ih/2:0:0,scale=iw:ih*2,setsar=1`), balloon "Ready for YouTube: yy_TB.mkv". No GPU is used. |
| `tests\run_yt8k.ps1` (eye_crop cases) | `WriteConfig` writes `eye_crop = none` by default so cases A-I keep their sizes. J / K / L: TALL eyes (1280x900 SBS = 640x900 eyes, 8K step, short side 432, tb_half) with `eye_crop` 16:9 -> frame 768x432 (eyes cropped to 640x360), 4:3 -> 576x432 (640x480), none -> 432x608, and K2 4:3pad -> 768x432 (640x480 padded to 854x480; picture 576 wide at x 96, `<name>_YT8K_4x3pad.mkv`, both bars black, the `pad=` filter in the logged command); each checks size, `top_bottom`, bottom half = right-eye colour and the logged `input .. eye_crop .. -> centre-cropped to ..; tb_half frame WxH` line (J also the `crop=iw/2:min(ih\,2*trunc(..))` expression). M: MKV step with 16:9 -> .mkv 640x360. Real 8192x4630 SBS (4096x4630 eyes), same graph: 7680x4320 (16:9), 5760x4320 (4:3), 4320x4884 (none). |
| `tests\run_hold.ps1` | The tray's `post_when` hold end to end, no GPU, no encoder, no real video: a scratch tray (`build\tt5`, `EYECAP_TRAY_TEST_SHM`, scratch `APPDATA`, `output_dir` = the scratch folder, `layout_3d = sbs_full` remux path, `keep_mp4 1`) with `ffmpeg_path` = `fake_ffmpeg.exe` (`tests/fake_ffmpeg.cpp`: works `FAKE_FFMPEG_MS` = 2500 ms in 50 ms ticks -- no progress while suspended -- then copies `-i` to the output; `-encoders` lists no nvenc), "recordings" = random bytes announced by `status_sim` (new `--linger <s>`: after releasing the block it stays alive, like a game that left VR). (A) `now`: `.mkv` made while the sim is live. (B) `game_exit`: block released, process lingers 6 s: no `.mkv` while it lives, `hold ON -- ... converts when the game closes` logged; made after it exits, `hold OFF` + producer exit logged. (C) `session_end`: queued but no `.mkv` while the session is live; made after release while the process still lives. (D) `session_end`, a second sim goes live while the fake ffmpeg runs: `SUSPENDED (hold)` logged, every thread of the fake ffmpeg `Wait/Suspended`, no `.mkv` while paused, `resumed` + `.mkv` after it ends, `MKV step ok in` > 5 s. (E) `manual`: the recording is queued (`queued while held -- converts when you say`), 4 s after the sim process exited there is still no `.mkv` and no fake ffmpeg; `WM_APP+7` (= tray menu "Convert last recording now", a test hook like `WM_APP+5/+6`) logs `convert now requested for ... (WM_APP+7)` and `... was waiting for the hold -- forced, runs now`, and the `.mkv` is made. (F) tray closed with `WM_CLOSE` while a job is paused: tray exits, no fake ffmpeg left, Stop()'s own `job: ffmpeg (pid <F's pid>) resumed (tray exiting)` before `job: cancelled (tray exiting)`, no `.mkv` / `.mkv.part`. (H0, first in H) picking an `.mkv` and a `_FLAT.mp4` is refused (`is not a recording`, balloon `Not a recording: hq_old.mkv, hq_old_FLAT.mp4`), nothing queued, both files intact. (I, after H) MKV + flat with `keep_mp4 0`, the tray KILLED (`Stop-Process`, no `Stop()`) during the flat step: the job object kills the fake ffmpeg; `queue.json` has `"own_format":1,"done_mkv":"hi_SBS.mkv"`; a planted `hi_SBS_FLAT.mp4.part` is deleted at the restart (`deleted stale partial output ... queue.json restored`), the job is restored `(MKV + 2D, none, sbs_full; already made: hi_SBS.mkv)`, `config.ini` changed meanwhile (flat off, keep MP4) but Start all makes only the flat file with the frozen snapshot (`MKV step already done before the restart`, no MKV cmd, no `hi_SBS (2).mkv`, no `.part` left, MP4 deleted, balloon names both files). (G) a new scratch tray started with `EYECAP_TEST_MIN_FREE_GB=100000`: the conversion is refused (`job: NOT STARTED hg_SBS.mp4: low disk space`, balloon "Not enough disk space"), no `.mkv`, no ffmpeg, MP4 kept. (D) waits for the `MKV step ok ... -> hd_SBS.mkv` line before reading its time (the fake ffmpeg's copy appears a moment before the tray logs it). (F) also checks that the interrupted job stays in `queue.json`; (G)'s new tray restores it (`queue: restored he_SBS.mp4 (settings of config.ini at its start) -- waits for you`). (H) the app window's queue, driven by `settings_test queue <pid> add|convert|start|startall|list` on the real window of a fresh scratch tray with `EYECAP_TRAY_TEST_PICK=<file>` (the "picked" MP4s, no file dialog), `post_when = now`, `FAKE_FFMPEG_MS = 6000`: "Add to queue..." three times for one MP4 with Eye crop none / 4:3 / 16:9 and Keep original MP4 cleared, never saved (`config.ini` keeps `eye_crop = none`, `keep_mp4 = 1`) -> three rows `hq_SBS.mp4  -  MKV, <crop>, sbs_full  -  waiting for you`; the same format again is refused (`is already waiting`); no ffmpeg and no `.mkv` 4 s later; `queue.json` has the three jobs; tray closed + restarted -> `restored 3 job(s)`, same rows; Start on row 2 (16:9) then Start all -> rows `none ... next`, `4:3 ... queued (started by you)`; run order 16:9, none, 4:3; `hq_SBS.mkv`, `hq_SBS (2).mkv`, `hq_SBS_4x3.mkv` exist; `keep_mp4 = 0, but 2 / 1 more queued job(s) need hq_SBS.mp4`, then `deleted hq_SBS.mp4`; `queue.json` empty. Not covered by a test: Remove with a multi-selection and Run now on a held running job (manual check). |
| tray post-processing | Manual (no GPU): `build\Release\status_sim.exe <private shm name> <hold s> <file.mp4>...` plays the layer's ready / recording / stopping / `saved <file>` sequence on a private status block; start a second tray on the same block with `EYECAP_TRAY_TEST_SHM=<name>` (and `APPDATA` pointing at a folder with a test `OpenXR-EyeCapture\config.ini`) from a copy of `EyeCaptureTray.exe` in a scratch folder (its `logs\tray.log` lands there). Check: `.mkv` made (ffprobe `Stereo 3D / side by side`, tag `stereo_mode=left_right`), MP4 deleted / kept per `keep_mp4`, `_FLAT.mp4` 3840x2160, `ffmpeg_path` pointing at a missing file = "ffmpeg not found" balloon and MP4 untouched, a garbage MP4 = failure balloon and MP4 untouched, two files in a row = FIFO. Dome (done 2026-10-10 with the `run_dome.ps1` recordings + their sidecars, `post_flat = 1`, `EYECAP_TRAY_TEST_NO_NVENC=1`): a `_VR180.mp4` via `saved <file>` and a `_VR360.mp4` dropped into the folder (fallback scan) -> `job: <name>.json -> VR180 / VR360`, MKV with `stereo_mode=left_right` / `top_bottom` and the spherical side data, `_FLAT.mp4` 3840x2160 (v360 flat view), MP4 and sidecar deleted, balloon "Ready for YouTube (VR180): ...". Note: `status_sim` crashes (0xC0000409) when `saved <path>` exceeds its 160-character message, so use a short folder (e.g. `build\tt`). |
| `tests\run_live.ps1` | Live share end to end, no OBS: `layer_e2e --record 0 --no-output` + `live_share_test.exe` (the plugin's `obs/live_consumer.cpp`) on a private block (`XR_EYE_CAPTURE_TEST_LIVE_SHM`); default the build DLL as an explicit layer (`-InstalledLayer` = the registered copy); each case pins its re-projection keys with `XR_EYE_CAPTURE_CONFIG`. Raw cases (D3D11 two / wide, D3D12 two / array; the consumer asks for raw eyes while the config says `symmetric_fov 1`, `head_smoothing 0.6`): producer found, eyes 1920x1080 B8G8R8A8_UNORM sRGB, `reprojected` 0, published rate ~60 at max fps 60 and ~30 at 30, both eyes' pixels = the test pattern bit-exactly (grey steps, primaries, eye id, ramp), a claim held for 0.6 s: the producer keeps publishing (>= 20 frames) and never re-publishes or rewrites that slot. Abandon case (D3D12): the consumer exits without `Close()` -> stop on the 2 s heartbeat timeout. Re-projection cases ("same as recording"): `d3d11_two_smooth` (`layer_e2e --jitter`: the mock's head jitter + 30 degree turn and asymmetric fovs, 1408x1440 eyes; `symmetric_fov 1`, `head_smoothing 0.6`, `output_fov_scale 0.9`, `edge_guard 1`): ring = `ComputeSymmetricFov` size (2 x 1164x1440), `reprojected` 3, the far marker at the same x / y in both halves (measured dx +0.13 px, dy +0.12 px), no black corner, the throttle and held-claim checks, then `--switch-raw`: a new generation with 1408x1440 raw eyes within 6 s (measured 1.2 s) and frames from it; log: `re-projected like the recording`, the re-projection summary (head smoothing on, symmetric fov on), `stopped (the viewer switched to raw eyes ...` and a raw `ring ready` after it. `d3d12_two_sym` (`symmetric_fov 1`, `head_smoothing 0`, the plain mock's symmetric +-0.8 rad identity views): the pass on a D3D12 game's raw ring, `reprojected` 1, the pattern within 2 (measured 0). `d3d11_two_cfgoff` (`symmetric_fov 0`, `head_smoothing 0`, "same as recording"): raw eyes, bit-exact, logged `raw eyes (symmetric_fov 0, head_smoothing 0: same as a recording)`. Every consumer case: layer log `consumer found`, `ring ready`, the last `stopped (consumer gone ...)` BEFORE the session ends, and every stop line with share calls > 0 and 0 game-thread share calls over 4 ms (measured max 0.7-2.0 ms). No-consumer and idle-block (exists, never heartbeated) cases: not one `live share` line. Protocol v3 cases: `d3d11_two` runs with `layer_e2e --overlays` and `record_overlays 1` while the consumer asks for raw eyes: still bit-exact, no overlay line (raw stays raw), and the in-headset indicator logs `state live`; `d3d11_wide` with `indicator_live 0`: the mock sees no quad layer; `d3d11_two_cfgoff` (no game quads): our live dot on 472 of 1260 frames (only while the viewer receives frames). `d3d11_two_half_raw` (`--raw --half`): ring 2 x 960x540, `reprojected` 4, `raw eyes, half size (identity pass)`, pattern within 2. `d3d12_two_half_sym`: `reprojected` 5, pattern within 2. `d3d12_two_ov_half` (`--overlays`, symmetric fov + smoothing 0.6, `output_fov_scale 1`, Half): the first ring has no overlays, `stopped (the game shows overlay layers ...)`, the next has `reprojected` 15; the overlay layers at the run_e2e positions (opaque quad, cylinder, back face not drawn) and the indicator region = the pattern; `live share: overlays: ... atlas` + `live share: overlay summary`. `d3d11_two_rec_live` (`--overlays --record 8`, the same config, full size): a recording and the live view at the same time -- the live frame checks as above (`reprojected` 11), the MP4 per `verify_mp4.ps1 -SkipPixels` (the overlays cover pattern steps) + the same overlay / indicator pixels in it, recorder `gpu busy 0, encoder stall 0, ring full <= 3` (measured 2: the same start-up drops as `run_e2e.ps1`'s overlay recordings without any live share, 2-4 there), both overlay summaries. Every consumer case also prints `layer_e2e`'s `E2E_ENDFRAME` (time inside `xrEndFrame`: capture + recording + live share + indicator + mock; measured max 0.6-2.0 ms, 0 frames over 4 ms in every case); the record + live case requires 0 over 4 ms after the first second. `-Only <regex>` runs a subset. Skips when `amtrucks` / `eurotrucks2` / `obs64` / `Beat Saber` runs. |
| `control_final_test.exe` | No GPU. The control thread + `ControlPublishFinal` on a private block (`XR_EYE_CAPTURE_TEST_SHM=<name>`): session destroyed right after the recording -> ownership released during the finalize -> final status published -> `ControlStop` at once: `saved <file>` + file + counters must survive; a Stopping -> Idle transition is published within 90 ms. |
| tray fallback scan | Part of the manual tray test above: `status_sim.exe <shm> <hold s> --vanish <file.mp4>` publishes `saved <file>` and clears ownerPid / state 50 ms later (the game quit right after the recording) -> the tray must still convert. Without any status message, drop a finished `*_SBS.mp4` / `*_VR180.mp4` / `*_VR360.mp4` (mtime newer than the tray start, no `.mkv`) into the tray's captures folder -> `tray: fallback scan queued <file>` within ~15 s and the MKV appears; a file still open for writing waits until it is closed. The same MP4 seen by both paths is converted once. `status_sim --outdir <dir>` publishes a captures folder. |
| tray IPC | Start `EyeCaptureTray.exe` (GUI), then `layer_e2e --record 0` (it must be started with the tray running, or with `XR_EYE_CAPTURE_REQUIRE_TRAY=0`) + `EyeCaptureTray.exe --start / --status / --stop`. |

Test-only environment variables read by the tray: `EYECAP_TRAY_TEST_SHM=<name>` (private status block, no single-instance check, no tray-alive marker, no registration flow), `EYECAP_TRAY_TEST_NO_NVENC=1` (the flat step uses libx264, the 8K step libx265; neither touches the GPU), `EYECAP_TRAY_TEST_YT8K_SHORT=<n>` (the 8K step uses n as the eye's short side instead of 4320; n even, 16..8192; logged as a test warning, invalid = ignored with a warning; `run_yt8k.ps1`), `EYECAP_TRAY_TEST_REG=1` (registration flow on in the `EYECAP_TRAY_TEST_SHM` mode; questions are logged and answered No) and `EYECAP_TRAY_TEST_REG_ROOTS=<lm key>;<cu key>` (both must start with `HKCU\`; replaces HKLM / HKCU for `--register` / `--unregister` / `--status` and the GUI; `run_install.ps1`), and `EYECAP_TRAY_TEST_PICK=<file>` (only with `EYECAP_TRAY_TEST_SHM`: "Convert a recording..." / "Add to queue..." take the picked MP4s from that file's lines, read at every click, instead of opening the Open dialog; `run_hold.ps1` H).

Test-only environment variables read by the layer: `XR_EYE_CAPTURE_TEST_LIVE_SHM=<name>` (live share block name instead of `Local\OpenXR-EyeCapture-Live`, logged; `run_live.ps1`), `XR_EYE_CAPTURE_TEST_NO_AV1=1` (`codec = av1`: the AV1 sink writer attempt fails in `CreateWriter` right after `AddStream(video)` (`why` = `test: XR_EYE_CAPTURE_TEST_NO_AV1`, E_FAIL) as if the GPU had no AV1 encoder, so the real fallback runs: file deleted, a second sink writer on the same path, the HEVC bitrate default, `AV1 unavailable -- fell back to HEVC`; `run_selftests.ps1` `*_noav1` asserts that line), `XR_EYE_CAPTURE_TEST_YT_SHORT=<n>` (`record_for_youtube`: the YouTube frame's short side n instead of 4320, 2..16384; logged as `XR_EYE_CAPTURE_TEST_YT_SHORT = n: YouTube frame short side n instead of 4320 (test override)`; `run_selftests.ps1` `*_yt_*`), `XR_EYE_CAPTURE_AUTORECORD2=<seconds>` (a second autorecord 1 s after the first one stopped; `run_reload.ps1`), `XR_EYE_CAPTURE_AUTORECORD=<seconds>` (start at the first frame,
stop after N s), `XR_EYE_CAPTURE_TRAY_IPC=0` (no control thread: no hotkey, nothing published to a running tray -- otherwise
the tray's post-processing would remux and delete the test MP4s; `run_e2e.ps1` / `run_jitter.ps1` set it for their
recording cases), `XR_EYE_CAPTURE_CONFIG=key=value;key=value` (applied on top of `config.ini`, same keys and checks,
each logged as `config override:`; `run_jitter.ps1` uses it for the smoothing settings, `run_dome.ps1` for the dome
settings, and `run_e2e.ps1` / `run_jitter.ps1` pin `projection=flat` so they test the flat path whatever `config.ini` says) and `XR_EYE_CAPTURE_TEST_SHM=<name>` (private status block + `<name>-Command` event; used by `control_final_test`), `XR_EYE_CAPTURE_OUTPUT_DIR=<dir>`, `XR_EYE_CAPTURE_REQUIRE_TRAY=0|1` (overrides `require_tray`; `run_e2e.ps1` sets 0 for the
recording cases).

## CI (GitHub Actions)

`.github/workflows/ci.yml`, one job on `windows-2022`. Triggers: pull requests to `master`, pushes to `master` and to
branches named `ci-*`, and manual runs (`workflow_dispatch`). A new push cancels the older run of the same ref.

**What runs**

1. Configure with `-G "Visual Studio 17 2022" -A x64 -DEYE_WERROR=ON -DEYE_BUILD_OBS_PLUGIN=ON` and build Release (the
   OBS plugin is built too, so the separate OBS zip cannot break unnoticed; it is not run, OBS is not installed). `EYE_WERROR` (default OFF, so local
   builds are unchanged) adds `/WX` to `EYE_WARN_FLAGS`: any compiler warning in our sources fails the build. The OpenXR SDK
   targets are not affected. To check locally: `cmake -B build -S . -DEYE_WERROR=ON`, build, then set it back to OFF.
2. Unit exes: `overlay_test`, `head_smoothing_test`, `indicator_test`, `install_test`, `control_final_test`.
3. `tests\run_hold.ps1` (fake ffmpeg), `tests\run_settings.ps1` (includes the real Settings dialog driven by `settings_test`),
   `tests\run_install.ps1 -AllowElevated`, `tests\run_yt8k.ps1 -FfmpegDir <dir>` (libx265 on the CPU),
   `tests\run_tag3d.ps1 -FfmpegDir <dir>` (Tag a video as 3D, a remux).
4. On failure the logs (`logs\*.log`, `build\ci_unit`, `build\tt*\**\*.log`, `build\settings_scratch`) are uploaded as the
   artifact `test-logs`.

**Notes**

- No test touches HKLM or the real HKCU `Run` key: registration tests redirect to `HKCU\Software\OpenXR-EyeCapture-Test`
  (`EYECAP_TRAY_TEST_REG_ROOTS`, `EYECAP_TRAY_TEST_RUN_KEY`), and `run_install.ps1` compares the real OpenXR keys before and after.
  "Start with Windows" also writes the `Explorer\StartupApproved\Run` record (12-byte REG_BINARY, first byte 2 = enabled) that Windows 11 needs before it starts a Run entry; the test redirect maps it to `<run test key>Approved`.
- `run_install.ps1` normally refuses to run elevated, so that an elevated `--register` could never write the real HKLM if
  the test redirection failed. GitHub runners are always an admin and are thrown away, so CI passes `-AllowElevated`;
  everything else in the script (the "registered=no" guard before any `--register`, the real-keys-unchanged check) still
  runs. Locally keep running it from a normal, non-elevated prompt.
- ffmpeg: `run_yt8k.ps1` takes `-FfmpegDir`, else `%EYECAP_FFMPEG_DIR%`, else the WinGet packages folder, else PATH. CI
  downloads the pinned `ffmpeg-<version>-essentials_build.zip` (gyan.dev build, via the GitHub release mirror) and sets
  `EYECAP_FFMPEG_DIR`. Bump `FFMPEG_VERSION` in the workflow to update it (the tray's own lookup is unchanged).
- The layer, the tray and the unit tests link Media Foundation (`mfplat.dll`); the first workflow step installs
  `Server-Media-Foundation` if the image does not have it.

**What does not run in CI, and why**

| Test | Why not |
|---|---|
| `run_e2e.ps1`, `run_selftests.ps1`, `run_jitter.ps1`, `run_dome.ps1`, `run_reload.ps1` | They record through the real `Recorder`, which needs hardware HEVC (and H.264) encoding through a Media Foundation hardware MFT on a D3D11 device. Hosted runners have no GPU, and Windows ships no software HEVC encoder MFT, so there is no software MFT / WARP route that exercises the same code. They also need the layer installed at the repo root and registered. They are not weakened to fit; run them locally before a release. |
| `layer_loadtest.exe` | Needs a real OpenXR runtime (VDXR here). |
| Anything with a real game or headset | See "Not verified without a headset". |

## Not verified without a headset

- A real game on a real runtime (VDXR / SteamVR / Oculus): swapchain images owned by those runtimes (keyed-mutex
  textures would be logged with `KEYED MUTEX`), real frame pacing, the beeps inside the headset.
- The hotkey path (`GetAsyncKeyState`) was not pressed in a test; the same start / stop code was driven through the tray IPC.
- Controller / wheel hotkeys (`tray/input.cpp`): device listing was checked on real hardware (`--inputs`: T300RS, T500 RS shifter, a generic USB joystick); a real button press firing a toggle, the XInput path with a connected pad and the capture of a pad / wheel chord in the app window were not tested by a script.
- Overlays (`record_overlays`) on a real game: the DLAA injector's panel / fps box placement, sharpness and blending in
  a real recording (the mock tests prove placement and blending math, not how a real VR panel looks), the injector's
  mode-2 draw (at the game's first release on the render thread) delivering the right image index, and the cost on a
  real 6120x6496-per-eye frame. Equirect / cube / MSAA / FP16 / 10-bit / colour-scale-bias / behind-projection layers
  are only tested on the mock runtime (no real game or runtime that submits them was tried); the cube face orientation
  follows the TextureCube rule, not checked against a real runtime; the cost of a full-eye ray draw (and of the
  behind-projection pass: two full-eye copies, a clear and a full-eye draw per eye) on an 8K-class frame is unmeasured.
- Head-locked overlays under head smoothing in a real ETS2 / ATS recording (the DLAA panel steady in the video): proven
  on the mock (`run_jitter.ps1` hud cases, `overlay_test`), not yet watched in a real recording. The pixel still has
  the sub-pixel shimmer of re-sampling the aliased panel edges (up to 0.8 px measured on the mock).
- YouTube's handling of the uploaded file (ffprobe parses the `st3d` box; YouTube's processing was not tried). Same for the
  VR180 / VR360 dome files (MP4 `sv3d` and the MKV `Projection` element are verified with ffprobe only).
- The dome with real head motion: translation (leaning) is ignored, so old painted areas can show small seams against
  the live view; whether `fill_fade_s` 8 / `window_feather_deg` 1 look right is a viewing judgement.
- Head smoothing with real head motion and a real runtime's view poses (the mock reports synthetic motion; the pass
  itself is runtime-independent). Whether the default strength / follow / fov scale feel right is a viewing judgement.
- Live view in a real OBS with a real game: the plugin's "Zoom" (plugin-only: built, never drawn in a test) and "Size",
  the overlays (DLAA panel) in the live view, the VRAM / GPU cost of the share pass + overlay atlases on ETS2-size eyes
  while recording, and the violet live dot in the headset (`indicator_live`; the mock only proves our quad layer is
  submitted in the live state).
