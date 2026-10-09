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
| `src/recorder.h/.cpp` | Recording: ring bookkeeping, video processor RGBA→NV12, linear→sRGB pre-pass, MF sink writer, AAC, finalize, st3d |
| `src/audio_capture.h/.cpp` | WASAPI loopback → continuous 48 kHz stereo int16 on the QPC clock |
| `src/st3d.h/.cpp` | Post-finalize MP4 patch: `st3d` box into the video sample entry |
| `src/control.h/.cpp` | Control thread: hotkey, tray IPC, status publishing, beeps |
| `src/shared_status.h` | Layer ↔ tray IPC names + layout (shared memory `Local\OpenXR-EyeCapture-Status`, event `...-Command`, tray-alive mutex `...-TrayAlive`) |
| `src/config.h/.cpp`, `src/log.h/.cpp` | `config.ini`, log files |
| `tray/tray.cpp` | `EyeCaptureTray.exe` |
| `tests/*` | See "Tests" |

## Data path

```
game thread, inside xrEndFrame (before forwarding):
  first XR_TYPE_COMPOSITION_LAYER_PROJECTION, views[0] = left, views[1] = right
  each view: subImage.swapchain -> tracked swapchain -> image = most recently RELEASED index
             (xrReleaseSwapchainImage releases the oldest acquired one), imageRect, imageArrayIndex
  Recorder::SubmitFrame(ts, L, R):
     free ring slot? (else drop, counted "ring full")
     IFrameSource::CopyEyes: GPU-wait(free fence) + copy L -> (0,0), R -> (leftWidth,0) + signal(ready fence)
       D3D11: CopySubresourceRegion x2 (+ ResolveSubresource for MSAA) on the game's immediate context, Flush
       D3D12: one command list on the game's queue: barriers RT->COPY_SOURCE, CopyTextureRegion x2, barriers back
  -> queue {slot, ready value, ts}
encoder worker thread (private D3D11 device on the game's adapter, by LUID):
  Wait(ready) -> [linear formats: compute pass, sRGB encode] -> VideoProcessorBlt RGBA->NV12 (BT.709 limited)
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
| `tests\run_e2e.ps1` | Real loader → installed layer → `mock_runtime.dll`, a normal app frame loop for every layout and both D3D APIs, recording through `XR_EYE_CAPTURE_AUTORECORD`; each MP4 verified. |
| dormant case (last case of `run_e2e.ps1`) | Without `XR_EYE_CAPTURE_REQUIRE_TRAY=0` and without the tray: `layer_e2e --expect-dormant` loop completes, no MP4, log has the "layer dormant" line and no `control:` / recorder lines. (Fails if a tray is running.) |
| tray IPC | Start `EyeCaptureTray.exe` (GUI), then `layer_e2e --record 0` (it must be started with the tray running, or with `XR_EYE_CAPTURE_REQUIRE_TRAY=0`) + `EyeCaptureTray.exe --start / --status / --stop`. |

Test-only environment variables read by the layer: `XR_EYE_CAPTURE_AUTORECORD=<seconds>` (start at the first frame,
stop after N s) and `XR_EYE_CAPTURE_OUTPUT_DIR=<dir>`, `XR_EYE_CAPTURE_REQUIRE_TRAY=0|1` (overrides `require_tray`; `run_e2e.ps1` sets 0 for the
recording cases).

## Not verified without a headset

- A real game on a real runtime (VDXR / SteamVR / Oculus): swapchain images owned by those runtimes (keyed-mutex
  textures would be logged with `KEYED MUTEX`), real frame pacing, the beeps inside the headset.
- The hotkey path (`GetAsyncKeyState`) was not pressed in a test; the same start / stop code was driven through the tray IPC.
- YouTube's handling of the uploaded file (ffprobe parses the `st3d` box; YouTube's processing was not tried).
