# SteamVR (OpenVR) recording — design

Status: design accepted 2026-10-17 (written by the Fable design pass, reviewed by the lead session). v1 in progress: part A (the proxy, `src\openvr\`) and part B (the tray window `tray\steamvr_ui.{h,cpp}` over the library `tray\steamvr_games.{h,cpp}`) are built and pass their mock tests; shipped in v0.5.0. Tested on a real headset with Crisis VRigade 2 (Unity, `IVRCompositor_026`, texture-array Submit, 72 Hz; Quest 3 via Virtual Desktop + SteamVR, per-game mode); quick mode not yet tried on a real headset.
Changed 2026-10-18 (user): the repair after game updates is "detect + ask with one click" (§4), no longer automatic.

## Decisions on the open questions (lead, 2026-10-17)
1. **No mirror-texture spike.** `IVRCompositor::GetMirrorTextureD3D11` gives the compositor's output (SteamVR UI and
   chaperone in it, no per-frame render pose, the compositor's pacing). The proxy records the game's own eye images.
2. **v1 ships without the in-headset dot.** Beeps + tray icon tell the state. The `IVROverlay` dot is phase 2.
3. ~~v1 enable flow = pick the game exe; Steam library auto-listing is phase 2.~~ Changed 2026-10-17 (user): Steam library
   auto-discovery and the repair after game updates are **v1** (see §4; since 2026-10-18 detect + one click, not automatic); "Add a game by exe..." stays for
   non-Steam games.
4. New user-visible strings use the new name **VR EyeCapture** (docs/RENAME_PLAN.md). Internal IDs keep the old
   spelling (`Local\OpenXR-EyeCapture-*`, `HKCU\Software\OpenXR-EyeCapture\...`), as the rename plan says.

Key facts the design rests on:
- `eye_capture_core` (static lib, `CMakeLists.txt`) already holds everything API-independent: recorder/encoder, D3D11/D3D12
  frame sources, reproject, dome, head smoothing, overlay compositor, indicator pixels + tracker, live share, audio, st3d,
  config, hotkey, log. Only `src\layer.cpp` and `src\control.cpp` are in the `eye_capture` DLL target, and `control.cpp`
  has no OpenXR in it (`tests\control_final_test.cpp` links it standalone).
- The front-end contract is small: `Recorder::Start(IFrameSource*, RecorderSettings, W, H, leftWidth, DXGI stage, srgb)` +
  `Recorder::SubmitFrame(ts100ns, EyeView L, EyeView R, displayTimeNs, overlays)` (`src\recorder.h`), with
  `EyeView{image, sc(SwapchainInfo*), swapchainKey, arrayIndex, x,y,w,h, hasPose, orientation, position, fov[4]}`
  (`src\frame_source.h`). `tests\encoder_selftest.cpp` already drives it without OpenXR.
- `D3D11Source::CopyEyes` (`src\source_d3d11.cpp`) only does Wait/CopySubresourceRegion/ResolveSubresource/Signal/Flush on
  the game's immediate context, so it can run inside an OpenVR `Submit` exactly as inside `xrEndFrame`.
- The layer glue that would otherwise be duplicated is ~250 lines of `layer.cpp`: `InitProcess` env parsing,
  `StartRecording`, `VideoClock`, `ReloadConfigForRecording`, `Snapshot/RequestStart/RequestStop`.

## 1. Interception point: per-game proxy `openvr_api.dll` + in-place vtable patch

**What ships:** `openvr_api.dll` (our proxy, built from `src\openvr\`) placed where the game's real DLL was; the Valve DLL
is renamed `openvr_api_real.dll` next to it; a sidecar `eye_capture_proxy.ini` next to both holds `home = <install dir>`
(where config.ini / logs / captures / the tray live) and `game = <display name>`. The proxy never links `openvr_api.lib`;
it resolves the real exports with `GetProcAddress` on `LoadLibraryW(<proxy dir>\openvr_api_real.dll)` at first use (never
in `DllMain`). Explicit trampolines (no `.def` forwarders) so a missing real DLL can be logged.

Exports (the stable export set of the real `openvr_api.dll`): `VR_InitInternal`, `VR_InitInternal2`, `VR_ShutdownInternal`,
`VR_GetGenericInterface`, `VR_IsInterfaceVersionValid`, `VR_GetInitToken`, `VR_IsHmdPresent`, `VR_IsRuntimeInstalled`,
`VR_RuntimePath`, `VR_GetVRInitErrorAsSymbol`, `VR_GetVRInitErrorAsEnglishDescription`, `VR_GetStringForHmdError`.
Before writing the .def, `dumpbin /exports` a real Valve DLL (e.g. `openvr_api.dll.steamvr-backup` in Crisis VRigade 2)
and forward EVERY export it has (it also has `VRHeadsetView`, `VRPaths`, `LiquidVR`, ... — XRSDKOpenVR.dll imports
`VRHeadsetView`; a missing one makes the game fail to load the plugin). Plus our own `EyeCaptureProxyVersion`.
Hooked: `VR_InitInternal2` / `VR_InitInternal` (banner, arm check), `VR_ShutdownInternal` (finalize, unpatch),
`VR_GetGenericInterface` (the interception). Everything else: straight forward.

**Inside `VR_GetGenericInterface(name, err)`** — both forms (Unity's native XRSDKOpenVR.dll asks for the C++
`"IVRCompositor_0xx"`; the C# side asks for `"FnTable:IVRCompositor_0xx"` / `"FnTable:IVRSystem_0xx"`):
- C++ interface: forward, then patch the returned object's **vtable in place** (`VirtualProtect` the two slots,
  `InterlockedExchangePointer`, restore protection; remember `{vtable, slot, original}` for unpatch; patch each distinct
  vtable once). Hooked slots: `WaitGetPoses` (slot 2) and `Submit` (slot 5). Not wrapper objects (40+ methods, 10+
  versions); not a cloned vtable on a proxy object (unhooked methods would get our `this`).
- FnTable: forward, **copy** the real function-pointer table (N entries for that version) into our own static table,
  replace the two entries, return the copy.
- Per-version slot table `kCompositorVersions[] = { "IVRCompositor_019", methodCount, slotWaitGetPoses, slotSubmit }, ...`
  for 019..028 in `src\openvr\openvr_versions.h`, from the `openvr.h` history. Slots 2 and 5 have been stable since 014;
  the method COUNT (for the FnTable copy) changes per version. Unknown version: one log line, forward untouched.
- `IVRSystem` is **not hooked**: the proxy asks the real DLL for its own `"IVRSystem_022"` (fallback 021/020/019) and
  calls `GetProjectionRaw`, `GetEyeToHeadTransform`, `GetFloatTrackedDeviceProperty(0, Prop_DisplayFrequency_Float)`.
- Reentrancy guard: `thread_local int depth`; an inner call is a plain pass-through (no double capture).

Not (b) global injection / vrclient hooking: needs an injector, trips anti-cheat/AV, breaks on SteamVR updates, cannot be
installed per game.

**Robustness / install rules (tray):**
- Steam "verify files" or a game update overwrites our `openvr_api.dll`: the game runs unrecorded. The tray list shows
  "proxy missing (game updated?) — Enable again", detected via the `EyeCaptureProxyVersion` export.
- Location detection: scan the chosen game folder recursively for 64-bit `openvr_api.dll` (PE machine AMD64) — Unity
  `<Game>_Data\Plugins\x86_64\`, UE4 `Engine\Binaries\ThirdParty\OpenVR\...\Win64\`, others next to the exe — install next
  to every hit. 32-bit copies: listed "not supported", skipped.
- If the folder has an `openvr_api_oc.dll` or the current `openvr_api.dll` is not Valve's (e.g. OpenComposite), the
  Enable dialog says so and offers to restore the `.steamvr-backup` / original first.
- EasyAntiCheat / BattlEye folders: warn, let the user decide.
- Original renamed in place (`MoveFileExW`); Disable = delete proxy + sidecar, rename back. The list
  `<install dir>\steamvr_games.ini` is bookkeeping only; the files on disk are the truth.

**Home discovery:** sidecar `eye_capture_proxy.ini`; fallback `HKCU\Software\OpenXR-EyeCapture\Home` (REG_SZ, written
non-elevated by the tray when the user enables a game, not at start). Neither → pure forwarder (no log, nothing created in the game folder).

## 2. Code reuse: keep `eye_capture_core`, move `control.cpp` into it, extract layer glue to `src\capture_common.{h,cpp}`

- CMake: add `src/control.cpp` and `src/capture_common.cpp` to `eye_capture_core`; drop `control.cpp` from the
  `eye_capture` DLL and from `control_final_test`'s own sources. New target `openvr_proxy` SHARED (`src/openvr/*.cpp`,
  `src/openvr/openvr_api.def`), `OUTPUT_NAME openvr_api`, links `eye_capture_core`. `scripts\package.ps1` puts it in the
  zip as `steamvr\openvr_api.dll` (never loose in the root).
- `src\capture_common.h` (pure functions):
  - `struct CaptureEnv {...}; CaptureEnv ReadCaptureEnv();` — the `XR_EYE_CAPTURE_*` env parsing of `InitProcess`, same
    env names so every test setup applies to the proxy.
  - `std::wstring RecordingFileName(const std::wstring& exeName, DomeProjection p)`.
  - `void FillRecorderSettings(const Config&, const EyeView& L, const EyeView& R, unsigned fps, RecorderSettings& out)`.
  - `unsigned PickFps(const Config&, double periodNs, int periodSamples, float refreshHz, const char** from)` + `SnapRate`.
  - `std::string ApplyReloadedConfig(Config& cur, const Config& fresh)` (returns the "changed ..." text).
  - `struct VideoClockAnchor { int64_t Map(int64_t displayTime, int* reanchorsLogged); }`.
- `layer.cpp` changes are mechanical call replacements; bar = behaviour-identical, verified by `run_e2e`, `run_reload`,
  `run_dome`, `run_jitter`, `run_live`, `run_selftests`.
- OpenVR front end per frame: two `EyeView`s (`image` = `ID3D11Texture2D*` from `Texture_t::handle`, `sc` = a
  `SwapchainInfo` from `GetDesc`, `swapchainKey` = texture pointer, `arrayIndex`, rect from bounds, pose/fov per §7), then
  `rec->SubmitFrame(QpcNow100ns(), L, R, 0, nullptr)` and `live->Frame(...)`. `FillRecorderSettings` + `Recorder::Start`
  at the first paired frame after a start request. `ControlStart(cfg, logPath, configPath, {Snapshot, RequestStart,
  RequestStop})` with `api = L"SteamVR D3D11"`, `runtime = L"SteamVR"` — hotkey, tray commands, beeps, status, require_tray
  for free.
- Risks: (1) `D3D11Source` caches resolve targets by `(swapchainKey, eye)`; OpenVR has no destroy event → call
  `ForgetSwapchain(ptr)` when `GetDesc` of a seen pointer changes (keep `{ptr,w,h,fmt,arraySize,samples}`). (2)
  `control.cpp` is a process singleton — one front end per process. (3) `VrIndicator` is OpenXR-only; only
  `RenderIndicator` / `IndicatorTracker` are reused (phase 2). (4) `settings_test` / `install_test` now link control.cpp.

## 3. VR indicator — phase 2
`IVROverlay_027` (fallback 026/025) from inside the game process: overlay `jrdevo.eyecapture.indicator`, HMD-relative
transform from `indicator_x/y/z`, 64x64 texture from `RenderIndicator`, Show/Hide from `IndicatorTracker`. Never in the
recording. `src\openvr\openvr_indicator.{h,cpp}`, ~200 lines.

## 4. Config / logging / tray
- `config.ini` `[steamvr]`: `steamvr_enabled = 1` (0 = pure forwarder), `steamvr_indicator = 1` (phase 2),
  `steamvr_pose_source = auto | submit | waitgetposes`. Everything else applies unchanged and reloads per recording.
  Commit config.ini only with new keys at code defaults (it is also the user's live config).
- Log: the same `<home>\logs\eye_capture.log` (pid on every line); banner
  `==== VR EyeCapture SteamVR proxy v<x> loaded into <exe> (pid) — real openvr_api_real.dll ...`.
- Tray: menu item **"SteamVR games..."** next to "Register for VR games..."; window `tray\steamvr_ui.{h,cpp}` over the
  UI-free library `tray\steamvr_games.{h,cpp}`. One row per game with a check box (ticked = the user enabled it) and a
  state: On / Off / Needs repair / OpenComposite installed / 32-bit, not supported (+ original missing / folder not
  found). Tick = Enable (confirmation with every DLL hit, 32-bit skipped, anti-cheat warning with No as default,
  OpenComposite: offer to restore SteamVR's DLL first), untick = Disable; buttons `Enable all` (Off games without
  anti-cheat), `Add a game by exe...` (non-Steam games), `Disable`, `Re-enable`, `Rescan`. README section "SteamVR games
  (Virtual Desktop + SteamVR)".
- **Steam library discovery (v1):** Steam folder = `HKCU\Software\Valve\Steam\SteamPath`, else
  `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`; libraries = that folder + `steamapps\libraryfolders.vdf` (new
  `"libraryfolders" { "0" { "path" ... } }` and old `"LibraryFolders" { "1" "D:\\..." }` formats; a small KeyValues
  parser); per library every `steamapps\appmanifest_*.acf` (appid, name, installdir; an installdir with `..` / a path
  separator is refused) → `ScanGame(<lib>\steamapps\common\<installdir>, depth 10, 60000 entries, 4 s)` → the games
  with an `openvr_api.dll` (64-bit folders and 32-bit-only folders kept apart). Runs on a worker thread when the window
  opens and on Rescan; the result is cached as `[found]` sections in `steamvr_games.ini` (the user's choices are the
  `[game]` sections; one lock around every read-modify-write). Tests: `EYECAP_TRAY_TEST_STEAM_DIR` (test-mode tray only;
  without it a test-mode tray sees no Steam at all), fixtures in `tests\fixtures\steam`.
- **Repair after updates (v1; detect + ask with one click since 2026-10-18 -- an automatic write into other apps' folders at
  start is what behaviour-based AV flags):** at tray start (posted after the icon exists) and when the window opens,
  `FindRepairs` walks ONLY the `[game]` entries, READ-ONLY: a folder in state ProxyMissing (Valve's DLL back, our
  `openvr_api_real.dll` / sidecar still there) = "game updated"; an Enabled folder whose sidecar `home` is not this install
  folder or no longer exists = "VR EyeCapture moved"; an Enabled folder whose proxy is older than the tray's (`VersionLess`
  on the `EYECAP_PROXY_VERSION=` marker) = "older recorder version". Every hit is logged in `tray.log`; at start one balloon
  "SteamVR recording: N game(s) need a repair after an update. Click to repair." (the names are in `tray.log` and the window) whose click (NIN_BALLOONUSERCLICK)
  opens the SteamVR games window: those rows show "Needs repair (<reason>)", and **Repair** (all of them) or **Re-enable**
  (one game) runs `RepairEnabledGames` / the Enable flow: the updated Valve DLL becomes the real one, the proxy is
  refreshed, the sidecar is rewritten with the new home and `HKCU\...\Home` refreshed (`EnsureHomeKey`). A folder whose
  `openvr_api.dll` / `openvr_api_real.dll` cannot be opened for writing (`FileInUse`: the running game has it mapped) is
  skipped (close the game, click Repair again); the export check of `EnableLocation` applies. Nothing is written to a game
  folder without the click.
- **Uninstall (v1):** **Disable all** in the window (one confirmation listing the enabled games); the tray's Unregister flow
  asks "Also put the original openvr_api.dll back in N SteamVR game(s)?" (Yes = the same). When the last enabled game is
  disabled the tray deletes the `HKCU\Software\OpenXR-EyeCapture` value `Home` (only when it names this folder or a
  folder that no longer exists).

- **Quick mode (v1, added 2026-10-17 at the user's request; decision 1 above stays true for per-game recording):**
  `steamvr_mode = per_game | quick | off` (`[steamvr]`, default per_game; off = the proxy is a pure forwarder too). Quick =
  `EyeCaptureMirror.exe` (`src\openvr\mirror_main.cpp`, links `eye_capture_core`), started by the tray while
  `vrserver.exe` runs (stopped through the event `Local\OpenXR-EyeCapture-MirrorQuit` when SteamVR exits or the mode
  changes). It loads SteamVR's OWN `openvr_api.dll` (`<runtime>\bin\win64\` of the first existing entry of "runtime" in
  `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`; changed 2026-10-18: no Valve binary is shipped any more; not found = exit code 4,
  one balloon "Quick mode needs SteamVR installed", no restart until the mode or SteamVR changes; tests:
  `XR_EYE_CAPTURE_MIRROR_OPENVR_DLL`, `XR_EYE_CAPTURE_TEST_OPENVRPATHS`), `VR_InitInternal2(VRApplication_Background)` (retried every 2 s while there is
  no server), asks EXACTLY `IVRSystem_022` (GetDXGIOutputInfo slot 7 -> its own D3D11 device on that adapter, the display
  frequency, PollNextEvent slot 29 -> exits on `VREvent_Quit`) and `FnTable:IVRCompositor_022` (openvr_capi.h's table of
  that version: GetCurrentSceneFocusProcess 22, GetMirrorTextureD3D11 33, ReleaseMirrorTextureD3D11 34; 41 entries -- the
  count `openvr_versions.h` already has for _022; _028 inserted SubmitWithArrayIndex at 6, so the old version is asked on
  purpose). Mirror textures once per run (re-asked after an error; one texture for both eyes = left / right half), a
  high-resolution waitable timer at the display frequency, the normal `Recorder::SubmitFrame` (flat SBS, `hasPose = false`;
  dome / head smoothing / symmetric_fov forced off, logged), live share, `ControlStart` with api "SteamVR mirror".
  Per game wins: the mirror is not "capable" while another live process owns the status block or the scene application
  (GetCurrentSceneFocusProcess) holds our proxy's marker `Local\OpenXR-EyeCapture-Proxy-<pid>` (created when the proxy
  claims the process at VR init) or the OpenXR layer's `Local\OpenXR-EyeCapture-Layer-<pid>` (created in the layer's
  `InitProcess`); checked with `OpenMutexW` only -- the mirror never opens or reads the game process (it used
  `PROCESS_VM_READ` + `EnumProcessModulesEx` before 2026-10-18). A running mirror recording is finalized then. Mirror exit
  code 3 = another instance holds `Local\OpenXR-EyeCapture-Mirror`: the tray retries after 10 s and does not count it toward
  its 3-failures back-off; while `steamvr_mode` is not quick and no mirror runs, the tray does no process snapshot at all. The choice is in the SteamVR games window (not the Settings window, which has no free row on
  a 1080p screen). Not tested against a real SteamVR: the slot numbers come from openvr_capi.h knowledge, the mock follows them.

## 5. Testing without a headset
- `tests\mock_openvr.cpp` → `mock_openvr.dll`, installed as `openvr_api_real.dll` next to the proxy in
  `build\steamvr_e2e\game\`: all exports; `IVRCompositor_028` and `_022` C++ objects with full-size vtables (unhooked slots
  = counting stubs) and `FnTable:` tables; `IVRSystem_022/019` (`GetProjectionRaw` asymmetric, e.g. L -1.1 R 0.9 T -1.0
  B 1.0 mirrored per eye; `GetEyeToHeadTransform` ±0.032 m; 90 Hz); `WaitGetPoses` = pose from `tests\test_motion.h`;
  `Submit` validates handle/bounds/flags/pose struct, counts per eye, returns None (`MOCK_OPENVR_SUBMIT_ERROR=n` returns
  an error). Prints `mock: submits L n R n, waitgetposes n, fntable submits n, errors n, interfaces asked: ...`.
- `tests\openvr_e2e.cpp` (fake game): loads `<dir>\game\openvr_api.dll`, D3D11 + `tests\test_pattern.h`,
  `--layout two|wide|array|msaa|typeless|fp16`, `--iface cpp|fntable`, `--pose submit|wait|none`, `--seconds`,
  `--submit-error`, `--out-dir`; prints `E2E_OUTPUT=<mp4>`.
- `tests\run_steamvr.ps1` (GPU, like `run_e2e.ps1`, same env setup): cases — every layout × cpp/fntable produce a valid
  MP4 (`verify_mp4.ps1`), log lines for load/patch/layout/record start, mock forwarded submit count == game submit count;
  pose submit vs wait give the `symmetric_fov` size; pose none → raw eyes; submit error passed through unchanged; dormant
  (no tray) → no record, counts intact; no sidecar → no log file; unknown version → "not recorded", game runs; pixel
  check of the eye-id bands (eye swap / bounds / fov sign).
- `tests\openvr_hook_test.cpp` (CPU, CI): version parsing, slot table, vtable patch/unpatch on a fake object, FnTable copy,
  `HmdMatrix34 → quat/pos`, `GetProjectionRaw → fov[4]`, bounds → rect, colour-space table, home discovery.
- `tests\steamvr_install_test.cpp` (CPU, CI): fake game tree, enable/disable/idempotent/proxy-missing/32-bit skipped/
  OpenComposite detected/file-in-use. Add both CPU tests to `.github\workflows\ci.yml`.

## 6. Phases
**v1:** D3D11, flat + symmetric_fov + head smoothing + dome, audio, hotkey/tray/beeps, MKV post-processing, live share,
tray enable/disable, Steam library discovery, automatic repair after game updates. Files: `CMakeLists.txt`, `src\capture_common.{h,cpp}`, `src\openvr\openvr_versions.h`,
`src\openvr\proxy_exports.cpp` + `openvr_api.def`, `src\openvr\openvr_hooks.{h,cpp}`, `src\openvr\openvr_capture.{h,cpp}`,
`third_party\openvr\openvr.h` (Valve header, BSD-3 → THIRD_PARTY notice; types only), `src\config.{h,cpp}`,
`tray\steamvr_games.{h,cpp}`, `tray\steamvr_ui.{h,cpp}`, `tray\settings.cpp`, `tray\tray.cpp`, tests above (+ `tests\run_steamvr_ui.ps1`,
`tests\fixtures\steam`), `scripts\package.ps1`, docs. ≈ 4k lines.
D3D12/Vulkan/OpenGL textures: detected, logged once as unsupported, forwarded.
**Phase 2:** IVROverlay indicator, depth submit structs, flipped-V bounds, D3D12. (Steam library auto-discovery moved to v1.)
**Planned, deliberately postponed until the real-headset test of v0.5.0** (from the 2026-10-18 audit):
- Change the interception method: instead of patching the runtime's `IVRCompositor` vtable in place (§1), hand the game a
  wrapper / a copied vtable for the C++ interface (as the FnTable path already does), so the runtime's own object is never
  written to. Waits for real SteamVR data on which games use which interface form.
- Move the keyboard / mouse hotkey polling out of the game process (proxy and layer `control.cpp` poll it today) into the
  tray, which already polls the controller / wheel bindings and sends `ECC_TOGGLE`; then no hotkey code runs inside the game.
**Phase 3:** OpenVR overlays into the recording, Vulkan, (32-bit not planned).

## 7. Pitfalls and their handling
- **Colour space:** `Gamma` → srgb; `Linear` → linear; `Auto` → `IsSrgbDxgi(format)`, else 8-bit UNORM = gamma, float /
  10-bit = linear. TYPELESS → `StageFormatForDxgi` family; MSAA resolve needs a TYPED format (`_UNORM_SRGB` if srgb else
  `_UNORM`). `gamma =` config still overrides.
- **Shared texture + bounds:** `x = uMin*W`, `w = (uMax-uMin)*W` → `ONE_SWAPCHAIN_TWO_RECTS`. Texture array:
  `arrayIndex = eye` when `ArraySize >= 2` (log once). `vMin > vMax`: v1 records with a warning (upside down).
- `Submit_LensDistortionAlreadyApplied`: warn once, still record. `Submit_TextureWithPose`: read
  `mDeviceToAbsoluteTracking`. Pose precedence: Submit pose > last `WaitGetPoses` render pose [0] (valid) > none.
  Eye pose = HMD × `GetEyeToHeadTransform(eye)` (right-handed, -Z forward, +Y up, same as OpenXR).
  FOV: `GetProjectionRaw` returns tangents with t negative / b positive → `fov = {atan(l), atan(r), atan(-t), atan(-b)}`;
  confirm the sign against the mock's asymmetric frustum in `run_steamvr.ps1`.
- **Lifetime:** copy before forwarding the second eye's `Submit`, on the game's immediate context. Never AddRef.
- **Pairing:** a frame = (Left, Right) in either order; a second Left before a Right replaces the pending one (counted);
  capture on the second eye. One-eye games: warn once, nothing recorded. Extra pairs: `max_fps` trims.
- **Threading:** one `std::mutex` around proxy state, `thread_local` reentrancy depth, SEH around the capture (a fault
  disables capture, the game continues), heap-leaked globals never destroyed at exit.
- **Device:** `QueryInterface(ID3D11Texture2D)` → `GetDevice` → `CreateD3D11FrameSource(dev)` at the first armed Submit.
- **Clock:** `ts100ns = QpcNow100ns()` at the second Submit, `displayTimeNs = 0`; fps = measured submit period (EMA,
  `SnapRate`) after 30 samples, else `Prop_DisplayFrequency_Float`, else config fps.
- OpenVR overlays / dashboard / chaperone: not in the submitted textures → not recorded.
- Exit without `VR_ShutdownInternal`: finalize lost like the OpenXR case; `ShutdownInternal` does Stop + WaitIdle(30 s)
  + ControlPublishFinal and unpatches.
