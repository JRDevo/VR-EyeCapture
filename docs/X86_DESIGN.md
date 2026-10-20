# 32-bit OpenXR games — design

Status: built 2026-10-20 on branch `feature/x86-games` (user decision "option A"); passes `tests\run_x86.ps1` (15 cases through
the real 32-bit OpenXR loader and a 32-bit mock runtime). **Not yet tried with a real 32-bit game / headset** (first target:
Overlord II with the user's 32-bit d3d9 proxy mod, VDXR 32-bit runtime).

## Problem

A 32-bit game loads the 32-bit OpenXR loader, which reads only the 32-bit registry view
(`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit`) and can only load a 32-bit layer DLL. The layer was
64-bit only and registered only in the 64-bit view, so such a game never loaded it. A 32-bit process also has 2 GB of address
space (Overlord II is not large-address-aware, and SteamStub prevents setting the flag): the encoder (Media Foundation, NVENC
MFT), WASAPI, the re-projection and a second D3D11 device do not belong inside it.

## Shape

```
32-bit game process                                         64-bit EyeCaptureRecorder.exe (started by the tray)
  eye_capture32.dll = src/layer.cpp (unchanged)               Recorder (src/recorder.cpp, unchanged): re-projection /
  + control thread: hotkey, tray status, cues                   symmetric_fov / head smoothing / dome, NVENC via MF,
  + config reload per recording, file name, indicator dot       WASAPI, st3d, sidecar
  + Recorder = src/recorder_remote.cpp ---- block ---->      src/recorder_helper.cpp: ring on the game's adapter,
      2 GPU copies per frame into a shared slot                 handles duplicated INTO the game, frames in order
      + frame descriptor (pose, fov, times)                     (CPU wait on `ready`), Signal(free)
```

* **One source, two builds.** `cmake -B build32 -A Win32` builds only `eye_capture32.dll` (+ its 32-bit tests): `src/layer.cpp`
  on a small core (log, config, hotkey, control, capture_common, D3D11 frame source, indicator, overlay geometry). Its
  `Recorder` is `src/recorder_remote.cpp` (same class, same header), so the layer code is the 64-bit one; the OBS live share
  and the D3D12 frame source are stubs (`src/x86_layer_stubs.cpp`). Static CRT (no x86 VC++ redistributable needed),
  `d3dcompiler_47.dll` delay-loaded (never used): the DLL imports only kernel32, user32, ole32, shell32, winmm.
* **The game keeps the control.** Hotkey polling, tray commands, the status block (the game pid owns it, so `post_when =
  game_exit` waits for the game as for 64-bit games), cues, the per-recording config reload and the in-headset dot run in the
  game exactly as in a 64-bit game; the status block's `api` says `D3D11 (32-bit)`. The helper is a remote encoder.
* **Protocol** (`src/x86_share_proto.h`, fixed layout pinned by static_asserts so the 32- and 64-bit builds agree):
  block `Local\OpenXR-EyeCapture-X86` created by the layer when an armed session gets its recorder; `-Wake` (auto-reset
  event, layer → helper), `-Quit` (tray → helper), `-Recorder` (helper single instance). Start: the RecorderSettings the
  layer filled (`EyeCapX86Start`) → the helper creates 3 shared SBS textures + shared `ready` / `free` fences on its own
  device on the game's adapter (LUID from the request), duplicates the NT handles into the game (`PROCESS_DUP_HANDLE`),
  starts its Recorder and acks when that is Recording. The layer opens them on the game's device
  (`IFrameSource::OpenRemoteRing`, on its link thread). Frame: the slot is reused only when `free` has completed the frame
  it held (CPU check: a busy slot drops the frame, counted — never a GPU or CPU wait on the helper); `D3D11Source::CopyEyes`
  (two copies + `Signal(ready)` + Flush, as for 64-bit games); a descriptor (slot, ready value, QPC time from the layer's
  video clock, displayTime, both eyes' rectangles / pose / fov) and `published`. The helper takes every published frame in
  order: CPU wait until `ready` completed (500 ms; a dead game cannot hang its GPU queue), `Recorder::SubmitFrame` from the
  slot (one GPU copy into the recorder's own ring on the helper's device), `Signal(free)`. Stop: the layer stops submitting,
  `stopSeq`; the helper drains, finalizes and reports `saved <file>` through the status part (seqlock).
* **Cost inside the game.** Per frame: two GPU copies + a signal + a flush (the same as a 64-bit game), one 160-byte
  descriptor, one SetEvent. Memory: the three ring textures are allocated by the helper (only opened in the game), two
  fences, the 5.4 KB block, a link thread per recording. Measured with `layer_e2e` (32-bit, 1920x1080 per eye): +5.8 MB of
  address space while recording (`run_x86.ps1` address_space).
* **Lifetime.** `producerActive` counts the layer's recorders; at 0 (the game's last armed session ended) the helper
  finishes and exits. The helper watches the game process handle: a game that exits or crashes mid-recording gets its MP4
  finalized by the helper, which then writes `saved <file>` into the tray's status block itself (the game's control thread
  is gone), so the tray converts it as usual. A helper killed mid-recording: the game runs on, the layer logs "recording
  lost". No helper: a start waits 8 s (the tray starts it within ~2 s of the block appearing), then fails with a clear
  message and the error cue; the game is unaffected (the wait is on the link thread).
* **Tray** (`tray.cpp` `X86Tick`, every 2 s): one `OpenFileMappingW` of the block — no process scan. Block present with a live,
  active producer and no helper → `EyeCaptureRecorder.exe --home <tray folder>` (logged with the game's name); exit code 3
  (another helper serves it) = retry in 10 s; 3 failures within a minute = no relaunch for that game (one balloon); a helper
  that outlives its game by 10 s gets `-Quit`. Exiting the tray does not stop the helper (a running recording continues, as
  inside a 64-bit game). A test-mode tray watches only `XR_EYE_CAPTURE_TEST_X86_SHM`.
* **Registration** (`tray/install.cpp`): `--register` writes `<dir>\XR_APILAYER_JRDEVO_eye_capture32.json = 0` in the 32-bit view
  (`KEY_WOW64_32KEY`) when `eye_capture32.dll` and that json are next to the exe, by the same rules as the 64-bit one (other
  same-name values removed, verified); `--unregister` / `--uninstall-cleanup` remove both; HKCU copies of either json are
  removed at tray start. The tray's state is "registered" only when both are (when the 32-bit files ship), so an update
  from 0.5.0 asks once ("can now also record 32-bit OpenXR games ... Register it now?"). `--status` adds `registered32=`.
  Never HKCU (HKCU\Software is the same key for both views: both loaders would see both manifests). Both manifests use the
  layer name `XR_APILAYER_JRDEVO_eye_capture` (one `disable_environment`); the views keep them apart.

## Limits (v1)

* Direct3D 11 only (a 32-bit D3D12 session is logged and not captured). Vulkan / OpenGL as for 64-bit games: not captured.
* Overlay layers (`record_overlays`) are not recorded for 32-bit games (logged once): only the projection layer's eyes cross
  the process boundary. The OBS live view is not available for 32-bit games.
* One 32-bit game at a time (a second one logs why it is not recorded and runs normally).
* A game running as administrator: the non-elevated helper can neither open the block nor duplicate handles into it (logged
  in both logs); run the game normally or the tray as administrator.
* Overlord II's mod renders one eye per frame (alternate-eye): the recording gets the two most recent eye images with their
  own render poses, as the headset does.

## Tests

`tests\run_x86.ps1` (GPU): layouts two / array / wide / msaa (pixel checks: eye ids, gamma, st3d, audio; drops ≤ 10 %; the
indicator quad), head smoothing + symmetric_fov (poses / fovs reach the helper), VR180 dome, the F24 hotkey, the tray's
ECC_TOGGLE command through the status block (state / api / "saved"), no helper, helper killed, game killed (helper finalizes
+ status block), two 32-bit games, a test-mode tray starting the helper, used address space, dormant. `install_test`
section 12 (32-bit view on test keys). CI builds the Win32 configure with `/WX`.
