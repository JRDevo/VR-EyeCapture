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
`scripts\package.ps1` builds Release and zips the release files into `dist\`. Our sources build with `/W3 /permissive-`
and zero warnings; `/EHa` everywhere (see "Fault handling").

`scripts\install.ps1` run from the repo copies `build\Release\{eye_capture.dll, json, EyeCaptureTray.exe}` to the repo
root (next to `config.ini`, git-ignored) and registers that json, so the repo root has the same layout as the release
zip. Re-run it after every build you want the games to pick up (a running game keeps the DLL locked).
`install.ps1` / `uninstall.ps1` are developer scripts and are not in the release zip: users run `EyeCaptureTray.exe`,
which registers the layer itself (see `tray/install.h/.cpp` below; same rules as the script) with one UAC prompt.

## Source map

| File | What |
|---|---|
| `src/layer.cpp` | Negotiation, `xrCreateApiLayerInstance`, `xrGetInstanceProcAddr` dispatch, all hooks, layout detection, frame clock, start logic |
| `src/frame_source.h/.cpp` | `IFrameSource` (graphics-API side of a capture), format helpers, private encoder device creation |
| `src/source_d3d11.cpp` | D3D11 games: ring created on the encoder device, opened on the game device; fences created on the game device |
| `src/source_d3d12.cpp` | D3D12 games: ring = D3D12 shared committed resources opened on the encoder device; D3D12 shared fences |
| `src/source_vulkan.cpp` | Stub (logs "not yet supported") |
| `src/recorder.h/.cpp` | Recording: ring bookkeeping, video processor RGBA→NV12, linear→sRGB pre-pass, head-smoothing re-projection pass or the dome pass, MF sink writer, AAC, finalize, st3d (+ sv3d and the `<name>.json` sidecar in dome mode) |
| `src/indicator.h/.cpp` | In-headset recording indicator: state tracker, 64x64 straight-alpha picture, the quad layer's swapchain / VIEW space (D3D11) |
| `src/head_smoothing.h/.cpp` | Head-motion smoothing, CPU side: orientation filter (follow, catch-up, roll lock, edge guard) and the per-eye re-projection parameters; quaternion helpers |
| `src/dome.h/.cpp` | VR180 / VR360 dome, CPU side: `[dome]` settings, output size (density, rounding, 8192 clamp), lon / lat -> ray, dome frame D (yaw only), per-eye matrix, direction -> equirect (tests). The GPU pass (`kDomePs`) is in `recorder.cpp` |
| `src/audio_capture.h/.cpp` | WASAPI loopback → continuous 48 kHz stereo int16 on the QPC clock |
| `src/st3d.h/.cpp` | Post-finalize MP4 patch: `st3d` box (+ `sv3d` for dome recordings) into the video sample entry |
| `src/control.h/.cpp` | Control thread: hotkey, tray IPC, status publishing, beeps. `ControlPublishFinal()` hands a finalized recording's result ("saved <file>") to the status block synchronously from `xrDestroySession` / `xrDestroyInstance` (the 250 ms loop is stopped with the last armed session); a Stopping -> Idle transition is also published at once. At shutdown only `ownerPid` / `state` are cleared, `message` / `file` stay so the tray still reads "saved <file>" after the game is gone |
| `src/shared_status.h` | Layer ↔ tray IPC names + layout (shared memory `Local\OpenXR-EyeCapture-Status`, event `...-Command`, tray-alive mutex `...-TrayAlive`) |
| `src/config.h/.cpp`, `src/log.h/.cpp` | `config.ini`, log files. `ParseConfigFile(path, cfg, problems)` = `LoadConfig`'s parser that also collects the "bad value" messages (used by the Settings window to validate a candidate file). The layer re-reads the file at every recording start: `ReloadConfigForRecording()` in `layer.cpp` (called from `CaptureImpl` before the `swap_eyes` decision, under `g_lock`) copies output_dir / codec / bitrate_mbps / fps / max_fps / max_width / gamma / swap_eyes / audio / `head` / `dome` / log_level over `g_cfg` after re-applying the `XR_EYE_CAPTURE_OUTPUT_DIR` and `XR_EYE_CAPTURE_CONFIG` overrides, and logs one `config reloaded for this recording from <path>: changed k=old->new, ...` (or `no changes`) line; an unreadable file keeps the old values (warning). hotkey, require_tray, indicator and post_* keep their game-start values |
| `tray/tray.cpp` | `EyeCaptureTray.exe`: tray icon / menu, status polling, queues finished recordings (`message` = `saved <file>` in the status block; the first read is only a baseline so older recordings are not converted), `logs\tray.log`. Fallback scan (game went away, and every 10 s while not recording): `*_SBS.mp4` / `*_VR180.mp4` / `*_VR360.mp4` in the captures folder that are newer than the tray start, have no `.mkv`, are not open for writing, and have a stable size for 3 s are queued (`tray: fallback scan queued <file>`); both paths dedupe by full path |
| `tray/install.h/.cpp` | Layer registration, registry only (no UI, no logging): `Query(exeDir)` -> `Registered` / `NotRegistered` / `StalePath` (a value with our json file name registered from another folder) / `DuplicateHkcu`, plus `hkcuDuplicate` and `filesPresent` (json + dll next to the exe). A value is "ours" when the last path component of its name equals `XR_APILAYER_JRDEVO_eye_capture.json` (case-insensitive, `\` or `/`), as in `install.ps1`; paths compare after `GetFullPathName` + lower case; ours with a non-zero DWORD counts as not registered. `Register` writes `<exeDir>\json = 0` to HKLM and deletes every other same-name value there (stale folder, case variant), then re-reads to verify; it opens the key for writing only when something must change, so an already-correct registration needs no admin. `Unregister` deletes every same-name HKLM value. `RemoveHkcuDuplicates` deletes same-name values from the CURRENT user's HKCU (no admin). Win32 error out (`ERROR_ACCESS_DENIED` -> "needs administrator rights", `ERROR_FILE_NOT_FOUND` -> files missing). 64-bit registry view. `SetTestRoots` redirects both roots for tests. Tray side (`tray.cpp`): CLI `--register` / `--unregister` (`RunRegCli`: log to `tray.log`, also cleans that account's HKCU; exit 0 / 1 / 5 = access denied), `--status` appends `registered=yes\|no\|stale` (+ `hkcu_duplicate=yes`, `files=missing`); GUI start: `RemoveHkcuDuplicates` + `Query`, then `WM_APP+4` -> `StartupRegCheck` (files missing -> error balloon, no prompt; not registered / stale -> `RegisterFlow` question). `RegisterFlow` / `UnregisterFlow`: `MessageBoxW` question -> `Elevate()` = `ShellExecuteExW` `runas` on its own exe with `--register` / `--unregister`, waits (pumping messages, 120 s cap) and reads the exit code; UAC declined (`ERROR_CANCELLED`) -> balloon; non-zero -> error box naming `logs\tray.log`; success -> `RemoveHkcuDuplicates` in the non-elevated parent (the elevated child may be another account) + `Query` + balloon. Cached `g_reg`, re-queried every 10 s in the timer, when the menu opens and after an action; `g_regBusy` blocks the menu while a question / UAC prompt is open. Not registered -> first tooltip line "Not registered for VR games (right-click: Register)" and the menu item "Register for VR games..." (else "Unregister from VR games..."), above Exit. The registration flow is off in the `EYECAP_TRAY_TEST_SHM` test mode unless `EYECAP_TRAY_TEST_REG=1`; in test mode questions are logged and answered No (no dialog, no UAC) |
| `tray/settings.h/.cpp` | The tray's **Settings...** window (plain `CreateWindowExW` controls on a popup window, nested message loop so the tray keeps running) and the config writer behind it. `SaveKeyValuesToFile` edits `ConfigPath()` in place: only `key = value` lines of known keys whose canonical value changed are rewritten (spelling, spacing, trailing comment, quotes, BOM and CR/LF kept); a missing non-default key is appended at the end of its `[section]` (section created if absent); the candidate is written to a temp file and checked with `ParseConfigFile` (only NEW problems block the save; `hotkey` is checked with `ParseHotkey` first); then temp-file + `MoveFileEx`. Errors go through one `Reporter` (MessageBox; in the tray's `EYECAP_TRAY_TEST_SHM` mode they are logged instead). `HotkeyText` maps a VK + modifiers to a name `ParseHotkey` accepts. Tray side: `Settings...` menu item, `WM_APP+3` opens the window (tests post it), `OnSettingsSaved` (tooltip override for 5 s via `kTipTimer`, `tray.log` line). Help: `kHelp[]` (control id -> text) feeds one `TOOLTIPS_CLASS` window (`TTF_IDISHWND \| TTF_SUBCLASS`, labels carry their control's id in `GWLP_USERDATA` and have `SS_NOTIFY`) and the read-only "What does this do?" edit (`ID_HELPTEXT`), updated from `WM_SETCURSOR` + a 100 ms hover timer, `EN/CBN/BN_SETFOCUS` and `CBN_SELCHANGE` of the projection combo; combo `Field`s have optional display `labels` (the file still gets the `options` values, mapped by index) |
| `tray/postprocess.h/.cpp` | Post-processing worker thread (FIFO, one job at a time): ffmpeg lookup, reads the recorder's `<name>.json` sidecar (`ReadSidecar`; none = flat SBS), MKV remux (`stereo_mode=left_right`, `top_bottom` for VR360), optional `_FLAT.mp4` (dome: `v360` flat view of the left eye), step 2b optional `_YT8K.mkv` (`post_yt8k`, flat SBS only, dome = logged skip: each eye `crop` -> `scale` into its own 3840x4320 box (lanczos, aspect kept, `force_divisible_by=2`) -> `pad` centred in that box -> `hstack` = 7680x4320 yuv420p, so both eyes keep identical geometry; `fps=60` in front when ffprobe (next to ffmpeg) reports `r_frame_rate` > 60; hevc_nvenc VBR 150 / max 200 Mbit/s else libx265 crf 18; Matroska `stereo_mode=left_right` written directly), MP4 (+ sidecar) delete only after verification, balloon notices. The flat and 8K steps share one hevc_nvenc detection (`-encoders`, once per job) and one nvenc-then-software runner. Config keys `post_mkv`, `keep_mp4`, `post_flat`, `post_yt8k`, `ffmpeg_path` (parsed in `src/config.cpp`, ignored by the layer). ffmpeg runs at below-normal priority with no window; exiting the tray kills it and deletes the partial output |
| `tests/*` | See "Tests" |

## Data path

```
game thread, inside xrEndFrame (before forwarding):
  first XR_TYPE_COMPOSITION_LAYER_PROJECTION, views[0] = left, views[1] = right
  each view: subImage.swapchain -> tracked swapchain -> image = most recently RELEASED index
             (xrReleaseSwapchainImage releases the oldest acquired one), imageRect, imageArrayIndex, pose, fov
  Recorder::SubmitFrame(ts, L, R):
     free ring slot? (else drop, counted "ring full")
     IFrameSource::CopyEyes: GPU-wait(free fence) + copy L -> (0,0), R -> (leftWidth,0) + signal(ready fence)
       D3D11: CopySubresourceRegion x2 (+ ResolveSubresource for MSAA) on the game's immediate context, Flush
       D3D12: one command list on the game's queue: barriers RT->COPY_SOURCE, CopyTextureRegion x2, barriers back
  -> queue {slot, ready value, ts}
encoder worker thread (private D3D11 device on the game's adapter, by LUID):
  Wait(ready) -> [linear formats: compute pass, sRGB encode] -> [re-projection pass (symmetric fov / smoothing) | dome pass]
  -> VideoProcessorBlt RGBA->NV12 (BT.709 limited)
  -> Signal(free) -> IMFTrackedSample(DXGI surface) -> IMFSinkWriter::WriteSample (hardware HEVC / H.264 MFT)
audio thread: WASAPI loopback -> int16 48 kHz -> WriteSample (AAC)
stop: drain, Finalize, st3d patch (+ sv3d, <name>.json sidecar in dome mode), summary line in the log
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
(source pixel density). The SBS output is `2 eyeW x eyeH`; the video processor scales it as before (8192 HEVC limit).
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
- **GPU pass** (`Recorder::Impl::Reproject`, encoder device, inside the device-manager lock): one full-screen triangle
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
- **Cost.** One extra W x H texture (+318 MB at 12240 x 6496) and the pass: 0.20 ms avg / 0.74 ms max GPU at
  12240 x 6496 on an RTX 5090 (D3D11 timestamp queries, logged), 0.01 ms worker CPU. The game thread's `SubmitFrame`
  is unchanged apart from copying 2 x 8 floats.
- **Stats** in `re-projection summary (head smoothing on|off, symmetric fov on|off)`: mean / max correction (angle O vs H), frames where the edge guard limited it,
  frames with uncovered output (black), frames with follow / catch-up active, frames without a usable pose (identity
  pass-through), filter resets, GPU / CPU cost.
- **Translation** is ignored on purpose (needs depth). Head-locked projection spaces (VIEW) give identity poses:
  nothing to smooth.

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
- **Sidecar** `<name>.json` (dome recordings only; no sidecar = flat SBS, so flat recordings are unchanged):
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
`Failed` = red X 2 s; otherwise hidden). Hidden = no call at all. Visible: on first use `VrIndicator` creates, through
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

### Timestamps and A/V sync

Video: `XrFrameEndInfo::displayTime` deltas, anchored to QPC at the first recorded frame (re-anchored if the two clocks
drift more than 0.5 s). Audio: WASAPI's per-packet QPC position, kept continuous (gaps filled with silence, overlaps
trimmed). Both are relative to the first video frame. Frame rate written to the stream: the measured submit rate of the
game (EMA of displayTime deltas, snapped to 45/60/72/80/90/120/144), else `XR_FB_display_refresh_rate` if the app
enabled it, else `config.ini fps`. Timestamps are the real ones either way (variable frame rate is preserved).

### st3d

Media Foundation writes `moov` after `mdat`; the patch appends the 13-byte `st3d` box (dome: the 111-byte `st3d` + `sv3d` blob, see above) (stereo_mode 2 = left-right) to
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

## Tests (no headset needed)

| Test | Proves |
|---|---|
| `tests\run_selftests.ps1` | `encoder_selftest` over D3D11/D3D12 x two/array/wide/msaa, unorm (linear), h264 and 3072x3264 per eye: the frame-source + recorder + audio + st3d path the layer uses, with a game-like device created without video support. Each file checked by `verify_mp4.ps1`. |
| `tests\verify_mp4.ps1` | ffprobe: codec, size, fps, duration, `Stereo 3D / side by side`, BT.709 tags, raw `st3d` bytes, AAC present / not silent; decoded pixels: grey steps within 6 code values in both eyes (gamma), left half = left eye. |
| `layer_loadtest.exe` | Real loader + active runtime (VDXR here): our layer is enumerated, negotiates, an instance is created and destroyed through it. |
| `tests\run_e2e.ps1` | Real loader → installed layer → `mock_runtime.dll`, a normal app frame loop for every layout and both D3D APIs, recording through `XR_EYE_CAPTURE_AUTORECORD`; each MP4 verified. Indicator: the mock validates every quad layer (known swapchain and space, rect inside the image, blend flag, unit quaternion) and prints the counts; D3D11 cases need a valid quad on >= the recorded frames and not on all frames, D3D12 none; an extra `vr_indicator = 0` case checks the recorded pixels where the dot would be (both eyes) equal the pattern and the indicator-on recording. |
| dormant case (last case of `run_e2e.ps1`) | Without `XR_EYE_CAPTURE_REQUIRE_TRAY=0` and without the tray: `layer_e2e --expect-dormant` loop completes, no MP4, log has the "layer dormant" line and no `control:` / recorder lines. (Fails if a tray is running.) |
| `indicator_test.exe` | Indicator state machine (starting / recording / stopping / saved 1.5 s / failed 2 s / hidden) and the picture (colours, sRGB vs linear, RGBA / BGRA, pulse 0.35..1.0, opacity cap, anti-aliased edge, X vs disc, hidden = transparent). |
| `head_smoothing_test.exe` | Symmetric fov intersection, output size (density, even rounding), parallel-camera mapping without smoothing; filter math on the CPU: time-constant mapping, first frame = identity, correction direction for yaw / pitch (OpenXR conventions), gap reset, roll lock, follow + catch-up (lag bound, settle), edge guard on / off, no-pose pass-through. Dome: size derivation (vr180 / vr360, rounding, 8192 clamp, explicit `dome_width`, no-fov fallback), lon / lat -> ray (lon +90 = +X, lat +90 = +Y, lon 0 = -Z), D = yaw only (pitch / roll dropped, straight-up degenerate case, slerp of the eyes), eye matrix for a yawed head, a world direction maps to the same equirect pixel for any head pose. |
| `tests\run_dome.ps1` | Dome end to end (installed layer, real loader, mock runtime `--jitter`: +-2 degree jitter + a 30 degree LEFT turn at t = 4..5 s, fovs L -50/+40 R -40/+50 x +-45), 11.5 s recordings `vr180` (last_seen, fade 8), `vr180_black`, `vr180_fade1` (`fill_fade_s 1`), `vr360`; Y plane decoded, marker located per eye, probe region at lon +33 / lat 0 (live before the turn, outside the window after it). (a) world lock: marker std-dev over t 1.0..3.9 s < 0.3 px, at lon = D yaw, lat 0 (2 deg). (b) mean after the turn = before (0.5 px); probe region still painted (last_seen) / black (fill black) at t ~ 5.5 s. (c) fade 1 s: painted at 5.5 s, black on every frame from 8.5 s. (d) right-eye marker = left-eye marker (mean <= 0.3 px, per-frame std-dev <= 0.15 px). (e) ffprobe on the MP4 and on the tray-command MKV: spherical (tiled) equirectangular + stereo side by side / top and bottom, the Matroska Projection element, the sidecar, the `_VR180` / `_VR360` names. Measured: (a) 0.058 / 0.085 px std-dev, lon -1.990 vs D yaw -1.99, lat -0.001; (b) 0.017 / -0.008 px; (d) mean 0.003 / -0.004 px, std-dev 0.09 px (per-frame max 0.32 px: each eye's own raster noise of the aliased test marker). |
| `tests\run_jitter.ps1` | Head smoothing end to end (installed layer, real loader, mock runtime): `layer_e2e --jitter` renders a world-locked grid + marker for the pose the mock reports (+-2 degree yaw / pitch / roll jitter at 3.3 / 4.1 / 4.7 Hz, a 30 degree yaw turn at t = 4..5 s; `tests/test_motion.h`). The mock's views have asymmetric fovs (left -50/+40, right -40/+50, +-45 degrees). Four recordings (smoothing 0 with `symmetric_fov` 1; smoothing 0 with `symmetric_fov` 0; 0.6 with `output_fov_scale` 0.9; same with `edge_guard` 0), ffmpeg-decoded, marker located per frame in both eyes. Checks: marker = analytic expectation (RMS < 0.15 degree), far marker at the same x in both halves with `symmetric_fov` 1 (<= 1 px) and not with 0, jitter-phase std-dev raw / smoothed >= 5, settles within 0.5 s after the turn (1 degree), final position 30 / 0 degrees, no black border pixels in the jitter phase. Measured: raw 2.00 degree, smoothed 0.31 degree (6.5x; 7.6x without the edge guard), settle 0.16 s (0.20 s without the guard), RMS 0.006 degree; far-marker x right - left: symmetric 0.29 px max (smoothed 0.50 px), raw asymmetric -244.5 px mean. |
| `tests\run_settings.ps1` | Settings window, no GPU, no game: (1) `settings_test hotkeys` -- every key name the window can produce (all VKs x 8 modifier sets, 736 texts) parses back through `ParseHotkey` to the same key + modifiers; (2) `settings_test save` -- the Save code path on a scratch copy of `config.ini`: `output_fov_scale = 7` and hotkey `Ctrl+Banana` refused (file byte-identical), unchanged values write nothing, bitrate / projection / hotkey changed in place (the before / after diff is exactly those 3 lines, comments intact), a key of a missing section is appended (CRLF kept); (3) `settings_test drive <pid>` -- a scratch tray (`build\tt2`, `EYECAP_TRAY_TEST_SHM`, scratch `APPDATA`) gets `WM_APP+3`, the real window is driven with `SendMessage` (Press-a-key captures F7, bad value refused and its message logged as `tray: settings message box [...]`, good values saved, untouched head_smoothing slider keeps the file's `0.57`), then `tray.log` must show the `settings saved ... changed ...` line, `tooltip now: settings saved -- applies to the next recording` and `tooltip restored` 5 s later. Only the scratch tray is stopped. |
| `tests\run_install.ps1` | Layer registration, no GPU, no game, no admin (refuses to run elevated); everything on test keys under `HKCU\Software\OpenXR-EyeCapture-Test` (deleted at start and end). (1) `install_test.exe`: NotRegistered -> Register -> exactly one value `<dir>\json` = DWORD 0 (another vendor's value and a look-alike `...json.bak` untouched), Register again = no change; a stale same-name value in another folder -> `StalePath` -> Register removes it; ours + a stale one with `/` and other case -> `StalePath` -> cleaned; ours = 1 (disabled) -> `NotRegistered` -> Register sets 0; ours in lower case -> `Registered`, Register rewrites the exact spelling; a CU copy -> `DuplicateHkcu` / `hkcuDuplicate`, `RemoveHkcuDuplicates` removes only it; Unregister removes every same-name value; missing json / dll -> `filesPresent` false, Register refuses with `ERROR_FILE_NOT_FOUND`; missing root key; a read-only key (protected DACL, read for Everyone = what a non-admin sees on HKLM) -> Register / Unregister `ERROR_ACCESS_DENIED` + "needs administrator rights", and an already-correct registration on it succeeds without writing. (2) a scratch `EyeCaptureTray.exe` in `build\tt3` with `EYECAP_TRAY_TEST_REG_ROOTS`: `--status` `registered=no` / `stale` / `yes` / `yes hkcu_duplicate=yes` / `files=missing`, `--register` exit 0 + value check, `--register` removes the CU copy, `--unregister` exit 0, no dll -> exit 1, read-only key -> exit 5, `tray.log` lines. (3) the same scratch tray as a GUI in test mode (`EYECAP_TRAY_TEST_SHM` + `EYECAP_TRAY_TEST_REG=1`): `tray.log` must show `registration: not registered`, the start question text answered No (test mode), `tooltip now: Not registered for VR games ...`; with a stale value the "registered from another folder" question; an outside `--register` is picked up by the 10 s re-query (`registration: registered (...)`, tooltip back to `no OpenXR game running`). (4) `build\Release\EyeCaptureTray.exe --status` on the REAL keys prints `registered=stale` (the repo root is the registered folder) and the real HKLM / HKCU OpenXR values are byte-for-byte unchanged. NOT covered: the real UAC / `runas` path and a real HKLM write. |
| `tests\run_reload.ps1` | Config reload at recording start (needs the installed layer; GPU-light, mock runtime; skips when `amtrucks` / `eurotrucks2` / `obs64` runs): scratch `APPDATA` config `projection = flat`, `bitrate_mbps = 20`; one `layer_e2e` process records 3 s, then (test-only `XR_EYE_CAPTURE_AUTORECORD2=3`) a second 3 s recording 1 s later; the script edits the config to `vr180` / `60` as soon as the first MP4 appears. Needs: `_SBS.mp4` then `_VR180.mp4`, log lines `config reloaded ...: no changes` then `...: changed bitrate_mbps=20->60, projection=flat->vr180`, recorder init lines `HEVC 20 Mb/s` then `HEVC 60 Mb/s`. (The MP4 bit rates themselves are only printed: the synthetic pattern compresses to ~1 Mb/s whatever the cap.) |
| `tests\run_yt8k.ps1` | The tray's 8K step (`post_yt8k`) end to end, CPU only (`EYECAP_TRAY_TEST_NO_NVENC=1` -> libx265, `EYECAP_TRAY_TEST_YT8K_SIZE=1536x864` -> each eye in a 768x864 box): a scratch tray (`build\tt4`, `EYECAP_TRAY_TEST_SHM`, scratch `APPDATA` config with `ffmpeg_path`) converts synthetic recordings that `status_sim` announces. (A) 1280x360 SBS (testsrc2 left, solid blue right), 30 fps, 2 s + AAC, `post_mkv 1` + `post_yt8k 1`, `keep_mp4 0`: `.mkv` + `_YT8K.mkv` made, MP4 deleted; `_YT8K.mkv` hevc 1536x864 yuv420p 30/1, `stereo_mode=left_right` tag + Stereo 3D side data, ~2 s, audio copied; average colour of each box's active area = that eye's source colour (within 10), black bars above / below; the logged command has the per-eye graph; balloon "Ready for YouTube: ..." + "YouTube 8K: ...". (B) 960x720 SBS (TALL 480x720 eyes), 72 fps, `post_mkv 0`: only `_YT8K.mkv`, 60/1 with 120 frames, log "capped to 60 fps" (`fps=60` in front of the graph), black bars on both sides of EACH eye including the centre seam (a whole-frame pad would fail this), balloon "Ready for YouTube (8K): ...". (C) `_VR180.mp4` + vr180 sidecar: "8K step skipped: VR180 ..." logged, nothing made, balloon "Nothing to convert". Measured on the real clip (by hand, hevc_nvenc, 2026-10-10): 5804x3278 @ 63.2 fps, 28.3 s video -> 7680x4320 @ 60, 148 Mbit/s video, 50 s at below-normal priority; each eye 3824x4320 with 8 px bars on both sides. |
| tray post-processing | Manual (no GPU): `build\Release\status_sim.exe <private shm name> <hold s> <file.mp4>...` plays the layer's ready / recording / stopping / `saved <file>` sequence on a private status block; start a second tray on the same block with `EYECAP_TRAY_TEST_SHM=<name>` (and `APPDATA` pointing at a folder with a test `OpenXR-EyeCapture\config.ini`) from a copy of `EyeCaptureTray.exe` in a scratch folder (its `logs\tray.log` lands there). Check: `.mkv` made (ffprobe `Stereo 3D / side by side`, tag `stereo_mode=left_right`), MP4 deleted / kept per `keep_mp4`, `_FLAT.mp4` 3840x2160, `ffmpeg_path` pointing at a missing file = "ffmpeg not found" balloon and MP4 untouched, a garbage MP4 = failure balloon and MP4 untouched, two files in a row = FIFO. Dome (done 2026-10-10 with the `run_dome.ps1` recordings + their sidecars, `post_flat = 1`, `EYECAP_TRAY_TEST_NO_NVENC=1`): a `_VR180.mp4` via `saved <file>` and a `_VR360.mp4` dropped into the folder (fallback scan) -> `job: <name>.json -> VR180 / VR360`, MKV with `stereo_mode=left_right` / `top_bottom` and the spherical side data, `_FLAT.mp4` 3840x2160 (v360 flat view), MP4 and sidecar deleted, balloon "Ready for YouTube (VR180): ...". Note: `status_sim` crashes (0xC0000409) when `saved <path>` exceeds its 160-character message, so use a short folder (e.g. `build\tt`). |
| `control_final_test.exe` | No GPU. The control thread + `ControlPublishFinal` on a private block (`XR_EYE_CAPTURE_TEST_SHM=<name>`): session destroyed right after the recording -> ownership released during the finalize -> final status published -> `ControlStop` at once: `saved <file>` + file + counters must survive; a Stopping -> Idle transition is published within 90 ms. |
| tray fallback scan | Part of the manual tray test above: `status_sim.exe <shm> <hold s> --vanish <file.mp4>` publishes `saved <file>` and clears ownerPid / state 50 ms later (the game quit right after the recording) -> the tray must still convert. Without any status message, drop a finished `*_SBS.mp4` / `*_VR180.mp4` / `*_VR360.mp4` (mtime newer than the tray start, no `.mkv`) into the tray's captures folder -> `tray: fallback scan queued <file>` within ~15 s and the MKV appears; a file still open for writing waits until it is closed. The same MP4 seen by both paths is converted once. `status_sim --outdir <dir>` publishes a captures folder. |
| tray IPC | Start `EyeCaptureTray.exe` (GUI), then `layer_e2e --record 0` (it must be started with the tray running, or with `XR_EYE_CAPTURE_REQUIRE_TRAY=0`) + `EyeCaptureTray.exe --start / --status / --stop`. |

Test-only environment variables read by the tray: `EYECAP_TRAY_TEST_SHM=<name>` (private status block, no single-instance check, no tray-alive marker, no registration flow), `EYECAP_TRAY_TEST_NO_NVENC=1` (the flat step uses libx264, the 8K step libx265; neither touches the GPU), `EYECAP_TRAY_TEST_YT8K_SIZE=<w>x<h>` (the 8K step makes a <w>x<h> frame, each eye in a <w/2>x<h> box, instead of 7680x4320; w a multiple of 4, h even; logged; `run_yt8k.ps1`), `EYECAP_TRAY_TEST_REG=1` (registration flow on in the `EYECAP_TRAY_TEST_SHM` mode; questions are logged and answered No) and `EYECAP_TRAY_TEST_REG_ROOTS=<lm key>;<cu key>` (both must start with `HKCU\`; replaces HKLM / HKCU for `--register` / `--unregister` / `--status` and the GUI; `run_install.ps1`).

Test-only environment variables read by the layer: `XR_EYE_CAPTURE_AUTORECORD2=<seconds>` (a second autorecord 1 s after the first one stopped; `run_reload.ps1`), `XR_EYE_CAPTURE_AUTORECORD=<seconds>` (start at the first frame,
stop after N s), `XR_EYE_CAPTURE_TRAY_IPC=0` (no control thread: no hotkey, nothing published to a running tray -- otherwise
the tray's post-processing would remux and delete the test MP4s; `run_e2e.ps1` / `run_jitter.ps1` set it for their
recording cases), `XR_EYE_CAPTURE_CONFIG=key=value;key=value` (applied on top of `config.ini`, same keys and checks,
each logged as `config override:`; `run_jitter.ps1` uses it for the smoothing settings, `run_dome.ps1` for the dome
settings, and `run_e2e.ps1` / `run_jitter.ps1` pin `projection=flat` so they test the flat path whatever `config.ini` says) and `XR_EYE_CAPTURE_TEST_SHM=<name>` (private status block + `<name>-Command` event; used by `control_final_test`), `XR_EYE_CAPTURE_OUTPUT_DIR=<dir>`, `XR_EYE_CAPTURE_REQUIRE_TRAY=0|1` (overrides `require_tray`; `run_e2e.ps1` sets 0 for the
recording cases).

## Not verified without a headset

- A real game on a real runtime (VDXR / SteamVR / Oculus): swapchain images owned by those runtimes (keyed-mutex
  textures would be logged with `KEYED MUTEX`), real frame pacing, the beeps inside the headset.
- The hotkey path (`GetAsyncKeyState`) was not pressed in a test; the same start / stop code was driven through the tray IPC.
- YouTube's handling of the uploaded file (ffprobe parses the `st3d` box; YouTube's processing was not tried). Same for the
  VR180 / VR360 dome files (MP4 `sv3d` and the MKV `Projection` element are verified with ffprobe only).
- The dome with real head motion: translation (leaning) is ignored, so old painted areas can show small seams against
  the live view; whether `fill_fade_s` 8 / `window_feather_deg` 1 look right is a viewing judgement.
- Head smoothing with real head motion and a real runtime's view poses (the mock reports synthetic motion; the pass
  itself is runtime-independent). Whether the default strength / follow / fov scale feel right is a viewing judgement.
