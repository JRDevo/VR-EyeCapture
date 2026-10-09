# OpenXR-EyeCapture — stereo recorder for any OpenXR game

Records **both eye images** of an OpenXR game, at exactly the resolution and quality the headset receives, into a
**side-by-side (left | right) 3D MP4** with the game's audio. Press a hotkey (or use the tray icon) to start and stop.
The MP4 carries the standard stereo flag (`st3d`, left-right), so YouTube and 3D players recognise it as 3D.

It is an **OpenXR API layer**: it sits between the game and the OpenXR runtime, so it works the same with
Virtual Desktop (VDXR), SteamVR, Oculus / Meta Link, or any other runtime. Nothing is injected into the game and no
game files are touched.

First tested target: **Euro Truck Simulator 2 / American Truck Simulator** (OpenXR beta, Direct3D 11) on Virtual
Desktop. Any other OpenXR game that renders with Direct3D 11 or Direct3D 12 should work the same way.

## Requirements

- Windows 10 (1703 or newer) or Windows 11, 64-bit.
- An OpenXR game using **Direct3D 11 or Direct3D 12**. Vulkan is planned ([`docs/VULKAN.md`](docs/VULKAN.md));
  OpenGL is not supported. Unsupported games run normally, just without recording.
- A GPU with a hardware HEVC (H.265) encoder: NVIDIA GTX 10xx or newer, AMD RX 400 or newer, Intel 7th gen or newer.
  HEVC is needed for wide frames: a Quest 3 at high settings is about 6000 x 3200 side by side, beyond H.264 limits.

## Install

1. Unzip `OpenXR-EyeCapture-v<version>.zip` to a folder you keep (the layer is loaded from there), for example
   `C:\Tools\OpenXR-EyeCapture`.
2. Right-click `scripts\install.ps1` → *Run with PowerShell* (or `powershell -ExecutionPolicy Bypass -File scripts\install.ps1`).
   It asks for administrator rights once and registers the layer for all OpenXR apps:
   `HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit\<folder>\XR_APILAYER_JRDEVO_eye_capture.json = 0`.
   No admin rights? `install.ps1 -CurrentUser` registers it for your user only (HKCU; ignored by games run as admin).
3. Put a shortcut to `EyeCaptureTray.exe` in `shell:startup` (Win+R, `shell:startup`) so it always runs. See "How it works".

Uninstall: `scripts\uninstall.ps1` (removes the registration; files and recordings stay).

## How it works: start the tray app first

**Start `EyeCaptureTray.exe` before launching the game. Without it the layer does nothing** (it is dormant: every
OpenXR call is passed straight through, no hotkey, no recorder, no encoder, no shared memory). While the tray app runs it
holds a small marker object (`Local\OpenXR-EyeCapture-TrayAlive`); the layer looks for it when the game creates its VR
session and arms itself only if it is there. The decision is made once per session, so starting the tray after the game
is already in VR does not arm it: restart the game (or its VR session). Closing the tray while a game is already armed
does not stop the layer; the hotkey keeps working until the game ends.
Set `require_tray = 0` in `config.ini` to always arm without the tray (old behaviour, hotkey only).

## Record

1. Start `EyeCaptureTray.exe`, then the game in VR as usual (ETS2 / ATS: launch option `-openxr`).
2. Press **Ctrl+Shift+F10** → one beep (880 Hz) in the headset: recording.
3. Press **Ctrl+Shift+F10** again → two beeps: stopped, the MP4 is finalized.
   One long low beep = it could not start (see the log).
4. The file lands in the `captures` folder next to `eye_capture.dll`, named
   `<game exe>_<yyyyMMdd_HHmmss>_SBS.mp4`, e.g. `eurotrucks2_20261009_201512_SBS.mp4`.

Recording also stops and the file is finalized when the game closes its VR session (quitting the game normally).
If the game crashes or is killed while recording, the MP4 is lost (it is only finalized at the end).

**Tray icon**: grey = no VR game, green = ready, red = recording, amber = starting / finalizing / failed.
Right-click: Start / Stop recording, Open captures folder, Open log, Edit config, Exit. The tooltip shows game,
resolution, fps, dropped frames and elapsed time. `EyeCaptureTray.exe --start | --stop | --toggle | --status` does the
same from a script or a Stream Deck button (`--start` / `--toggle` print an error if the tray app is not running).

## Upload to YouTube

Upload the MP4 as it is. YouTube reads the stereo flag (Spherical Video V2 `st3d`, left-right) and processes it as a 3D
video; viewers in a headset (YouTube VR) get 3D. Processing of large files takes a while; the 3D version can appear later
than the first standard-definition version.

If YouTube shows it as a flat side-by-side video after processing has finished, make an MKV copy (no re-encode, takes
seconds) and upload that instead:

```
powershell -ExecutionPolicy Bypass -File scripts\make_mkv.ps1 captures\eurotrucks2_20261009_201512_SBS.mp4
```

This needs ffmpeg (`winget install Gyan.FFmpeg.Essentials`); the MKV gets the Matroska `stereo_mode = left_right` tag.

## Settings — `config.ini`

Next to `eye_capture.dll` (or `%APPDATA%\OpenXR-EyeCapture\config.ini`, which wins if it exists). Read when a game
starts its OpenXR instance.

| Key | Default | Meaning |
|---|---|---|
| `hotkey` | `Ctrl+Shift+F10` | Start / stop. `Ctrl`, `Shift`, `Alt` + `F1`..`F24`, `A`..`Z`, `0`..`9`, `Numpad0`..`9`, `Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`; `none` = tray only. Modifiers must match exactly. |
| `output_dir` | *(empty)* = `captures` next to the DLL | Where MP4s go. |
| `codec` | `hevc` | `hevc` or `h264` (H.264 is limited to 4096 px wide; wider frames are scaled down). |
| `bitrate_mbps` | `0` = 120 (HEVC) / 80 (H.264) | Video bitrate. |
| `fps` | `90` | Fallback frame rate if it cannot be measured (it normally is: the game's real frame rate). |
| `max_fps` | `0` | 0 = every frame the game renders; e.g. `60` drops frames above 60 fps. |
| `max_width` | `0` | 0 = native resolution; otherwise the side-by-side frame is scaled down to this width. |
| `gamma` | `auto` | `auto` from the swapchain format; `srgb` / `linear` only if colours look washed out or too dark. |
| `audio_enabled` | `1` | Record the sound (what plays on the playback device). |
| `audio_device` | *(empty)* = default playback device | Part of the device name, e.g. `Virtual Desktop Audio`. |
| `require_tray` | `1` | `1` = the layer is only active while `EyeCaptureTray.exe` is running (checked when the game creates its VR session). `0` = always active. |
| `log_level` | `info` | `error`, `warn`, `info`, `debug`. |

## Log and troubleshooting

`logs\eye_capture.log` next to the DLL, with a copy in the output folder. Every OpenXR app that starts writes a block:
the game exe, runtime, graphics API (`capture AVAILABLE` / `DISABLED`), every swapchain (format, size, array, MSAA),
the frame layout it detected (`TWO_SWAPCHAINS`, `TEXTURE_ARRAY`, `ONE_SWAPCHAIN_TWO_RECTS`), the encoder it picked
(e.g. `NVIDIA HEVC Encoder MFT [hardware]`) and a summary per recording (frames written / dropped, duration, file).

- **Nothing happens on the hotkey**: first make sure `EyeCaptureTray.exe` was running before the game started; if not the
  log says `EyeCaptureTray not running ... layer dormant for this session`. Then check the log has a `session ... created: graphics API D3D11 -> capture AVAILABLE`
  line for the game. No `loaded into <game>.exe` line at all = the layer is not registered (run `install.ps1`) or the
  game does not use OpenXR.
- **Dropped frames** in the summary: lower `max_fps` or `max_width`, or `bitrate_mbps`.
- **Disable without uninstalling**: set the environment variable `DISABLE_XR_APILAYER_JRDEVO_eye_capture=1`.

## Cost

Per frame the game's render thread does two GPU copies (plus an MSAA resolve when the game uses MSAA swapchains),
a fence signal and nothing else — measured 0.07 ms (D3D12) / 0.18 ms (D3D11) of CPU time per frame on an RTX 5090.
Encoding runs on its own threads and its own D3D11 device on the same GPU. Extra VRAM while recording: about
3 x the side-by-side frame in RGBA plus 4 x in NV12 (≈ 400 MB at 6144 x 3264). Nothing is done while not recording.

## With other mods

- **ETS2 / ATS DLAA + DLSS injector**: works alongside; this layer never hooks Present or the game's D3D calls. The
  default hotkey is Ctrl+**Shift**+F10 because plain Ctrl+F10 is the injector's snapshot key.
- **Virtual Desktop's own OpenXR layer** and other implicit layers coexist (each registers its own entry).

## Building from source

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). Short version: Visual Studio 2022, then
`powershell -ExecutionPolicy Bypass -File scripts\package.ps1`.
