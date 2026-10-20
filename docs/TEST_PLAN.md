# Test plan — new features after v0.5.0 (2026-10-20)

Real-hardware checks that the automated suites cannot do. Each feature already passes its automated tests
(mock runtime / mock SteamVR); this list is what still needs a person, a headset and a real game.

Setup for every test: install the new `dist\VR-EyeCapture-Setup-v<version>.exe`, start the tray app,
Quest 3 through Virtual Desktop. After each test, keep `logs\eye_capture.log` and `logs\tray.log` and note the
recording file name.

Result per test: PASS / FAIL + one line of what you saw.

## 1. Auto-update (tray)

| # | Do | Expect |
|---|---|---|
| 1.1 | Start the tray with `check_updates = 1`. Wait 10 s. | `tray.log`: one update check, "up to date" (v0.5.0 is the latest). No balloon. |
| 1.2 | App window → Recording → **Check now**. | Message "up to date". |
| 1.3 | Untick **Updates**, Save. Restart the tray. | `tray.log`: no update check. |
| 1.4 | After the next GitHub release: start an older install. | One balloon "vX.Y.Z is available", tray menu item **Update to vX.Y.Z...**, line in the app window. Restart the tray: no second balloon. |
| 1.5 | Click **Update to vX.Y.Z...** → confirm. | Download, then the Windows admin prompt, then the setup. Tray closes, new version starts. |
| 1.6 | Pull the network cable, click **Check now**. | Short error, nothing breaks, log line. |
| 1.7 | Bitdefender stays quiet during 1.1 – 1.5. | No quarantine, no ATD alert. |

## 2. Direct3D 12 SteamVR games (per game)

Game: **Resident Evil 2** with REFramework in OpenVR mode (`openvr_api.dll` in the game folder,
`re2_config.ini` → `TargetPlatform=DirectX12`).

| # | Do | Expect |
|---|---|---|
| 2.1 | Tray → **SteamVR games...** → **Add a game by exe...** → `re2.exe`. | RE2 ticked; original `openvr_api.dll` kept as backup. |
| 2.2 | Start RE2 in VR. Wait in the first room. | Game runs as before. Log: "SteamVR D3D12", direct queue. Red dot visible in the headset. |
| 2.3 | Press the hotkey, record 30 s with head movement, stop. | Dot red while recording. MP4 saved. Both eyes correct (left = left), no black frames, no stutter in the game. |
| 2.4 | Record 5 min. | Dropped frames in the log near 0. Game frame rate unchanged (compare with recording off). |
| 2.5 | Head smoothing 0.6 and VR180 dome: one short recording each. | Works like a D3D11 game. |
| 2.6 | OBS: add the **VR EyeCapture** source while RE2 runs. | Live picture; dot turns violet. |
| 2.7 | Untick RE2 in **SteamVR games...**. | REFramework's original `openvr_api.dll` back; RE2 VR works without us. |
| FAIL signs | Crash, "device removed", black or garbled video. | Send the log: probably the hand-off state (see STEAMVR_DESIGN §8). |

## 3. `tag_3d` switch and new bitrates (issue #1)

| # | Do | Expect |
|---|---|---|
| 3.1 | Export → untick **3D tag**, record 10 s. | MP4 opens in DaVinci Resolve as a plain video; `ffprobe` shows no st3d / sv3d. |
| 3.2 | Tick it again, record 10 s. | YouTube / the Quest player see 3D again. |
| 3.3 | With 3D tag off and `post_mkv = 1`. | The MKV still has `stereo_mode`. |
| 3.4 | Bitrate 500 Mbit/s, 1 min recording. | File size about 3.75 GB/min; no dropped frames on the RTX 5090. |

## 4. 32-bit OpenXR games (Overlord II mod) — after branch `feature/x86-games` is merged

| # | Do | Expect |
|---|---|---|
| 4.1 | Install; check that both 64-bit and 32-bit layer entries exist (WOW6432Node). | One entry each, never also in HKCU. |
| 4.2 | Start Overlord II with the VR mod (`O2VR.ini` `Mode=mirror` first). | Game runs as before; the tray starts the 64-bit recorder helper; dot visible. |
| 4.3 | Hotkey, 30 s, stop. | Video saved, picture correct. Game memory use (Task Manager) only slightly higher. |
| 4.4 | Kill the helper during a recording. | The game keeps running. |
| 4.5 | Later, with the mod's true stereo: record again. | Proper 3D. (`Mode=aer` draws one eye per frame, so the 3D looks wrong there; that is expected.) |
| 4.6 | 64-bit game (ATS) still records. | No change. |

## 5. Vulkan — planned, not built

When it is built (see `VULKAN_PLAN.md` §5): `hello_xr -g Vulkan2`, a Godot 4 XR demo, a UE5 VR template with
the Vulkan RHI, X-Plane 12 (OpenXR); DOOM VFR, No Man's Sky (SteamVR). Same checks as section 2:
game unchanged, eyes correct, dropped frames, dot, OBS live view.

## 6. Regression (every release)

| # | Do | Expect |
|---|---|---|
| 6.1 | ATS / ETS2 OpenXR (D3D11): 1 min recording. | Same as v0.5.0. |
| 6.2 | A D3D11 SteamVR game per game (e.g. Crisis VRigade 2) and quick mode. | Same as v0.5.0. |
| 6.3 | Settings window on a 1080p screen. | Everything fits, nothing cut. |
| 6.4 | Uninstall through Windows Settings → Apps. | Layers gone (64 and 32 bit), game DLLs restored. |
