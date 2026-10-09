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
`encoder_selftest.exe`, `layer_loadtest.exe`, `mock_runtime.dll` (+ `mock_runtime.json`), `layer_e2e.exe`.
`scripts\package.ps1` builds Release and zips the release files into `dist\`. Our sources build with `/W3 /permissive-`
and zero warnings; `/EHa` everywhere (see "Fault handling").

`scripts\install.ps1` run from the repo copies `build\Release\{eye_capture.dll, json, EyeCaptureTray.exe}` to the repo
root (next to `config.ini`, git-ignored) and registers that json, so the repo root has the same layout as the release
zip. Re-run it after every build you want the games to pick up (a running game keeps the DLL locked).

## Source map

| File | What |
|---|---|
| `src/layer.cpp` | Negotiation, `xrCreateApiLayerInstance`, `xrGetInstanceProcAddr` dispatch, all hooks, layout detection, frame clock, start logic |
| `src/frame_source.h/.cpp` | `IFrameSource` (graphics-API side of a capture), format helpers, private encoder device creation |
| `src/source_d3d11.cpp` | D3D11 games: ring created on the encoder device, opened on the game device; fences created on the game device |
| `src/source_d3d12.cpp` | D3D12 games: ring = D3D12 shared committed resources opened on the encoder device; D3D12 shared fences |
| `src/source_vulkan.cpp` | Stub (logs "not yet supported") |
| `src/recorder.h/.cpp` | Recording: ring bookkeeping, video processor RGBA→NV12, linear→sRGB pre-pass, head-smoothing re-projection pass, MF sink writer, AAC, finalize, st3d |
| `src/indicator.h/.cpp` | In-headset recording indicator: state tracker, 64x64 straight-alpha picture, the quad layer's swapchain / VIEW space (D3D11) |
| `src/head_smoothing.h/.cpp` | Head-motion smoothing, CPU side: orientation filter (follow, catch-up, roll lock, edge guard) and the per-eye re-projection parameters |
| `src/audio_capture.h/.cpp` | WASAPI loopback → continuous 48 kHz stereo int16 on the QPC clock |
| `src/st3d.h/.cpp` | Post-finalize MP4 patch: `st3d` box into the video sample entry |
| `src/control.h/.cpp` | Control thread: hotkey, tray IPC, status publishing, beeps. `ControlPublishFinal()` hands a finalized recording's result ("saved <file>") to the status block synchronously from `xrDestroySession` / `xrDestroyInstance` (the 250 ms loop is stopped with the last armed session); a Stopping -> Idle transition is also published at once. At shutdown only `ownerPid` / `state` are cleared, `message` / `file` stay so the tray still reads "saved <file>" after the game is gone |
| `src/shared_status.h` | Layer ↔ tray IPC names + layout (shared memory `Local\OpenXR-EyeCapture-Status`, event `...-Command`, tray-alive mutex `...-TrayAlive`) |
| `src/config.h/.cpp`, `src/log.h/.cpp` | `config.ini`, log files |
| `tray/tray.cpp` | `EyeCaptureTray.exe`: tray icon / menu, status polling, queues finished recordings (`message` = `saved <file>` in the status block; the first read is only a baseline so older recordings are not converted), `logs\tray.log`. Fallback scan (game went away, and every 10 s while not recording): `*_SBS.mp4` in the captures folder that are newer than the tray start, have no `.mkv`, are not open for writing, and have a stable size for 3 s are queued (`tray: fallback scan queued <file>`); both paths dedupe by full path |
| `tray/postprocess.h/.cpp` | Post-processing worker thread (FIFO, one job at a time): ffmpeg lookup, MKV remux (`stereo_mode=left_right`), optional `_FLAT.mp4`, MP4 delete only after verification, balloon notices. Config keys `post_mkv`, `keep_mp4`, `post_flat`, `ffmpeg_path` (parsed in `src/config.cpp`, ignored by the layer). ffmpeg runs at below-normal priority with no window; exiting the tray kills it and deletes the partial output |
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
  Wait(ready) -> [linear formats: compute pass, sRGB encode] -> [head_smoothing > 0: re-projection pass]
  -> VideoProcessorBlt RGBA->NV12 (BT.709 limited)
  -> Signal(free) -> IMFTrackedSample(DXGI surface) -> IMFSinkWriter::WriteSample (hardware HEVC / H.264 MFT)
audio thread: WASAPI loopback -> int16 48 kHz -> WriteSample (AAC)
stop: drain, Finalize, st3d patch, summary line in the log
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

Media Foundation writes `moov` after `mdat`; the patch appends the 13-byte `st3d` box (stereo_mode 2 = left-right) to
the video sample entry (`hvc1` / `avc1`), grows every ancestor's size and rewrites only the moov region in place.
`moov`-before-`mdat` files are handled too (shrink a following `free` box, else a rewrite with every `stco`/`co64` +13)
but Media Foundation never produced one in the tests.

### Dormant mode (tray required)

`EyeCaptureTray.exe` (GUI mode only, not the `--start/--stop/--toggle/--status` CLI) holds the named mutex
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
| `head_smoothing_test.exe` | Symmetric fov intersection, output size (density, even rounding), parallel-camera mapping without smoothing; filter math on the CPU: time-constant mapping, first frame = identity, correction direction for yaw / pitch (OpenXR conventions), gap reset, roll lock, follow + catch-up (lag bound, settle), edge guard on / off, no-pose pass-through. |
| `tests\run_jitter.ps1` | Head smoothing end to end (installed layer, real loader, mock runtime): `layer_e2e --jitter` renders a world-locked grid + marker for the pose the mock reports (+-2 degree yaw / pitch / roll jitter at 3.3 / 4.1 / 4.7 Hz, a 30 degree yaw turn at t = 4..5 s; `tests/test_motion.h`). The mock's views have asymmetric fovs (left -50/+40, right -40/+50, +-45 degrees). Four recordings (smoothing 0 with `symmetric_fov` 1; smoothing 0 with `symmetric_fov` 0; 0.6 with `output_fov_scale` 0.9; same with `edge_guard` 0), ffmpeg-decoded, marker located per frame in both eyes. Checks: marker = analytic expectation (RMS < 0.15 degree), far marker at the same x in both halves with `symmetric_fov` 1 (<= 1 px) and not with 0, jitter-phase std-dev raw / smoothed >= 5, settles within 0.5 s after the turn (1 degree), final position 30 / 0 degrees, no black border pixels in the jitter phase. Measured: raw 2.00 degree, smoothed 0.31 degree (6.5x; 7.6x without the edge guard), settle 0.16 s (0.20 s without the guard), RMS 0.006 degree; far-marker x right - left: symmetric 0.29 px max (smoothed 0.50 px), raw asymmetric -244.5 px mean. |
| tray post-processing | Manual (no GPU): `build\Release\status_sim.exe <private shm name> <hold s> <file.mp4>...` plays the layer's ready / recording / stopping / `saved <file>` sequence on a private status block; start a second tray on the same block with `EYECAP_TRAY_TEST_SHM=<name>` (and `APPDATA` pointing at a folder with a test `OpenXR-EyeCapture\config.ini`) from a copy of `EyeCaptureTray.exe` in a scratch folder (its `logs\tray.log` lands there). Check: `.mkv` made (ffprobe `Stereo 3D / side by side`, tag `stereo_mode=left_right`), MP4 deleted / kept per `keep_mp4`, `_FLAT.mp4` 3840x2160, `ffmpeg_path` pointing at a missing file = "ffmpeg not found" balloon and MP4 untouched, a garbage MP4 = failure balloon and MP4 untouched, two files in a row = FIFO. |
| `control_final_test.exe` | No GPU. The control thread + `ControlPublishFinal` on a private block (`XR_EYE_CAPTURE_TEST_SHM=<name>`): session destroyed right after the recording -> ownership released during the finalize -> final status published -> `ControlStop` at once: `saved <file>` + file + counters must survive; a Stopping -> Idle transition is published within 90 ms. |
| tray fallback scan | Part of the manual tray test above: `status_sim.exe <shm> <hold s> --vanish <file.mp4>` publishes `saved <file>` and clears ownerPid / state 50 ms later (the game quit right after the recording) -> the tray must still convert. Without any status message, drop a finished `*_SBS.mp4` (mtime newer than the tray start, no `.mkv`) into the tray's captures folder -> `tray: fallback scan queued <file>` within ~15 s and the MKV appears; a file still open for writing waits until it is closed. The same MP4 seen by both paths is converted once. `status_sim --outdir <dir>` publishes a captures folder. |
| tray IPC | Start `EyeCaptureTray.exe` (GUI), then `layer_e2e --record 0` (it must be started with the tray running, or with `XR_EYE_CAPTURE_REQUIRE_TRAY=0`) + `EyeCaptureTray.exe --start / --status / --stop`. |

Test-only environment variables read by the tray: `EYECAP_TRAY_TEST_SHM=<name>` (private status block, no single-instance check, no tray-alive marker) and `EYECAP_TRAY_TEST_NO_NVENC=1` (the flat step uses libx264 and never touches the GPU).

Test-only environment variables read by the layer: `XR_EYE_CAPTURE_AUTORECORD=<seconds>` (start at the first frame,
stop after N s), `XR_EYE_CAPTURE_TRAY_IPC=0` (no control thread: no hotkey, nothing published to a running tray -- otherwise
the tray's post-processing would remux and delete the test MP4s; `run_e2e.ps1` / `run_jitter.ps1` set it for their
recording cases), `XR_EYE_CAPTURE_CONFIG=key=value;key=value` (applied on top of `config.ini`, same keys and checks,
each logged as `config override:`; `run_jitter.ps1` uses it for the smoothing settings) and `XR_EYE_CAPTURE_TEST_SHM=<name>` (private status block + `<name>-Command` event; used by `control_final_test`), `XR_EYE_CAPTURE_OUTPUT_DIR=<dir>`, `XR_EYE_CAPTURE_REQUIRE_TRAY=0|1` (overrides `require_tray`; `run_e2e.ps1` sets 0 for the
recording cases).

## Not verified without a headset

- A real game on a real runtime (VDXR / SteamVR / Oculus): swapchain images owned by those runtimes (keyed-mutex
  textures would be logged with `KEYED MUTEX`), real frame pacing, the beeps inside the headset.
- The hotkey path (`GetAsyncKeyState`) was not pressed in a test; the same start / stop code was driven through the tray IPC.
- YouTube's handling of the uploaded file (ffprobe parses the `st3d` box; YouTube's processing was not tried).
- Head smoothing with real head motion and a real runtime's view poses (the mock reports synthetic motion; the pass
  itself is runtime-independent). Whether the default strength / follow / fov scale feel right is a viewing judgement.
