# OpenXR-EyeCapture — stereo recorder for any OpenXR game

Records **both eye images** of an OpenXR game, at exactly the resolution and quality the headset receives, into a
**side-by-side (left | right) 3D MP4** with the game's audio. Press a hotkey (or use the tray icon) to start and stop.
The MP4 carries the standard stereo flag (`st3d`, left-right) for 3D players; for YouTube the tray app also converts every
recording to a `.mkv` with the Matroska stereo tag, which YouTube reliably recognises as 3D (see "Upload to YouTube").

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
   `<game exe>_<yyyyMMdd_HHmmss>_SBS.mp4`, e.g. `eurotrucks2_20261009_201512_SBS.mp4` (`_VR180.mp4` / `_VR360.mp4` in
   dome mode, see "VR180 / VR360 dome mode").

Recording also stops and the file is finalized when the game closes its VR session (quitting the game normally).
If the game crashes or is killed while recording, the MP4 is lost (it is only finalized at the end).

**Tray icon**: grey = no VR game, green = ready, red = recording, amber = starting / finalizing / failed,
blue = converting a finished recording (see "Upload to YouTube").
Right-click: Start / Stop recording, Open captures folder, Convert last recording now, Open log, Edit config, Exit. The tooltip shows game,
resolution, fps, dropped frames and elapsed time, and "converting <file> (MKV)..." / "last: <file>.mkv ready". `EyeCaptureTray.exe --start | --stop | --toggle | --status` does the
same from a script or a Stream Deck button (`--start` / `--toggle` print an error if the tray app is not running).

## Upload to YouTube

**The tray app converts every recording to a `.mkv` automatically; upload the `.mkv`.** YouTube only reliably recognises
the 3D tag as the Matroska `stereo_mode = left_right` tag (it ignored the MP4 `st3d` box), so when a recording is saved
the tray (it must be running, which it is anyway for the layer to be active) remuxes the MP4 with ffmpeg: no re-encode, takes
seconds, and the picture is untouched. YouTube then processes it as a 3D video; viewers in a headset (YouTube VR) get 3D.
Processing of large files takes a while; the 3D version can appear later than the first standard-definition version.

- While it runs the tray icon is **blue** and the tooltip says "converting <file> (MKV)..."; afterwards the tooltip says
  "last: <file>.mkv ready" and a balloon says "Ready for YouTube: <file>.mkv". One recording at a time, in order.
- The MP4 is deleted only after the MKV exists and is larger than 90% of the MP4 (`keep_mp4 = 1` keeps both). If anything
  fails (a balloon shows the first line of ffmpeg's error) the MP4 is left untouched. **Tray menu > Convert last recording
  now** retries the most recent recording (it needs the MP4, so use `keep_mp4 = 1` if you want to be able to redo it).
- Needs ffmpeg: `winget install Gyan.FFmpeg.Essentials`. The tray finds it on `PATH`, else in the WinGet packages folder, else
  in an `ffmpeg` folder next to the tray app (or set `ffmpeg_path`). Without ffmpeg you get one balloon "ffmpeg not found"
  and the MP4 stays as it is.
- Dome recordings (`_VR180` / `_VR360`): the same, the MKV keeps the VR180 / 360 metadata (`stereo_mode` is `top_bottom`
  for VR360). See "VR180 / VR360 dome mode".
- Only recordings that finish while the tray runs are converted; older ones: `scripts\make_mkv.ps1 <file.mp4>` (the manual
  path, same result) or "Convert last recording now".
- `post_flat = 1` also writes `<name>_FLAT.mp4` (normal 2D video: left eye, 16:9 centre crop, 3840 wide, NVENC HEVC or x264;
  same as `scripts\make_flat.ps1`) for viewers without a headset. The tray log is `logs\tray.log` (each ffmpeg command line,
  duration and result).

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
| `vr_indicator` | `1` | Small recording dot in the headset (never in the video). See "In-headset indicator". |
| `indicator_x` / `_y` / `_z` | `0.22` / `-0.16` / `-0.8` | Dot position in metres relative to your head (right / up / forward is negative z). |
| `indicator_size` | `0.02` | Dot size in metres. |
| `indicator_opacity` | `1.0` | Maximum opacity of the dot (0..1). |
| `symmetric_fov` | `1` | Re-project both eyes to one common symmetric view (standard 3D SBS). See "Why the eyes are re-projected to a symmetric view". `0` = raw eye images. |
| `head_smoothing` | `0` | Head-motion smoothing strength, `0` = off. See "Head-motion smoothing". |
| `head_follow_deg` | `12` | Smoothing: how far (degrees) the view may lag a deliberate turn before it catches up. |
| `roll_lock` | `0` | Smoothing: `1` = keep the horizon level. |
| `output_fov_scale` | `0.9` | Smoothing: fraction of the game's field of view in the recording (room for the correction). |
| `edge_guard` | `1` | Smoothing: `1` = never show black edges (the correction is limited to what the game rendered). |
| `projection`, `dome_width`, `dome_forward`, `fill`, `fill_fade_s`, `window_feather_deg` | | `[dome]`: VR180 / VR360 output, see "VR180 / VR360 dome mode". |
| `post_mkv` | `1` | Tray app: after each recording, remux the MP4 to `<name>.mkv` with the Matroska 3D tag (what YouTube reads). |
| `keep_mp4` | `0` | Tray app: `0` = delete the MP4 once the MKV (and the flat file, if requested) is verified; `1` = keep both. |
| `post_flat` | `0` | Tray app: also write `<name>_FLAT.mp4`, a normal 2D video (left eye, 16:9 centre crop, 3840 wide). |
| `ffmpeg_path` | *(empty)* = auto | Tray app: full path of `ffmpeg.exe`. Empty = `PATH`, then WinGet packages, then an `ffmpeg` folder next to the tray app. |
| `require_tray` | `1` | `1` = the layer is only active while `EyeCaptureTray.exe` is running (checked when the game creates its VR session). `0` = always active. |
| `log_level` | `info` | `error`, `warn`, `info`, `debug`. |

## In-headset indicator

While a recording runs you see a small dot at the lower right of your view, in the headset only: **amber** while the
recording starts (and while it is being finished), **red, pulsing** while recording, **green** for 1.5 s when the file
was saved, a **red X** for 2 s if it failed. Otherwise nothing is shown and nothing is done. The dot is a separate
OpenXR layer that the layer adds after it has copied the game's eye images, so it never appears in the video.
Settings in `config.ini` `[indicator]`: `vr_indicator = 0` turns it off; `indicator_x/y/z` move it (metres, relative
to your head), `indicator_size` and `indicator_opacity` change size and maximum opacity. Direct3D 11 games (ETS2 / ATS)
only for now; on Direct3D 12 the log says `indicator: not available on D3D12 yet` and recording works as before.
If the runtime ever rejects the extra layer, the indicator switches itself off for the session and the game's frame is
submitted unchanged (logged).

## Head-motion smoothing

A recording shows exactly where your head pointed, so every small shake and nod is in the video. Viewers who watch it
on a flat screen or in their own headset do not move with you, and the shaking is tiring to watch. With
`head_smoothing` set, the recorder re-projects each eye image onto a smoothed head orientation, the way a headset's
timewarp re-projects a frame: small fast movements are removed, slow and deliberate ones are kept. Only the
recording is affected; the game and your headset view are untouched, and nothing extra runs on the game's thread (the
work is done on the encoder's own GPU device, after the frame is copied).

- **`head_smoothing`** (0 = off, the default): strength. **0.5 to 0.7 is recommended**; 0.6 removes roughly 80 to 85 % of
  a 3 to 5 Hz shake. Higher values make the camera feel heavier.
- **`head_follow_deg`** (default 12): when you deliberately look somewhere else (a mirror, a junction), the smoothed
  view follows as soon as it is this many degrees behind and then catches up quickly, so it does not drift after you.
- **`roll_lock`** (default 0): `1` keeps the horizon level by removing head tilt from the recording.
- **`output_fov_scale`** (default 0.9): the recording shows 90 % of the game's field of view (a slight zoom, the
  outer border is trimmed). That border is the room the correction needs: rotating the view would otherwise show the
  black area outside what the game rendered. Lower it (e.g. 0.85) for stronger smoothing with fewer limits; 1.0 trims
  nothing but leaves no room. Only used while smoothing is on.
- **`edge_guard`** (default 1): never show black edges. When a correction would reach past the rendered image (big
  shakes, the start of a fast turn, `roll_lock` with a tilted head) the view follows your head just enough to stay
  inside it. `0` applies the full correction and lets black edges appear.

Only rotation is smoothed; small position shakes are not visible at normal viewing distances and cannot be
re-projected without depth. If a game submits its views in a head-locked space, there is nothing to smooth (the log
line `re-projection summary` then shows a correction of 0). The summary line per recording shows the mean / max
correction, how often the edge guard limited it, frames with black edges, and the GPU cost.

### Why the eyes are re-projected to a symmetric view

A headset renders each eye with an asymmetric field of view: the outer side is wider than the inner side (for example
-50 / +40 degrees for the left eye and -40 / +50 for the right). The headset's compositor knows that and shows each
image at the right place. A 3D side-by-side player (YouTube VR, a 3D TV) does not: it puts both halves on the same
rectangle, so a far-away object appears at different positions in the two eyes, as if it were closer than infinity or
"beyond" it. The eyes try to fuse that and cannot -- eye strain, double images. Standard 3D video needs two parallel
cameras with identical symmetric fields of view.

With `symmetric_fov = 1` (the default) the recorder therefore re-projects both eyes (on the encoder's GPU device, the
same pass as head smoothing) to the part of the view both eyes share, symmetric and identical for both (in the example
+-40 degrees horizontally), facing the same direction. Eye positions are kept, so the 3D depth is unchanged. The video
is a little narrower than the raw eye images (it keeps the game's pixel density, e.g. 5058 instead of 6120 pixels per
eye for a -50 / +40 view) and costs one GPU pass per frame. `symmetric_fov = 0` records the raw eye images as before.
The log shows the numbers at the start of each recording, e.g.
`symmetric fov: source L[-50.0,+40.0]x[-45.0,+45.0] R[-40.0,+50.0]x[-45.0,+45.0] -> output ±40.0° x ±45.0° per eye, 5058x6496 per eye`.

## VR180 / VR360 dome mode

`[dome] projection = vr180` (or `vr360`) records a **VR180 (or 360) 3D video** instead of a flat side-by-side one: in
YouTube VR the viewer is placed *inside* the scene and can look around, instead of watching a rectangle that follows
your head.

**Mental model: the dome is a world-locked canvas; only what you looked at is painted; stale areas fade.** At the start
of a recording the recorder fixes a "dome" around you, facing the way your head pointed (level with the horizon). Every
frame, each eye's image is painted onto that dome at the place it was actually looking. Your head moving does not move
the picture (head shake simply disappears), and when you look left the left part of the dome gets painted. What you
looked at earlier stays visible (`fill = last_seen`) until it is older than `fill_fade_s` seconds, then fades to black
over 2 s; areas you never looked at are black. The live view always has the game's full resolution and the stereo of
the two eyes (each eye's dome is centred on that eye, like a VR180 camera).

- **Layout.** VR180: two square half-spheres (180 x 180 degrees), left eye left, right eye right. VR360: two 2:1 full
  spheres, left eye on top, right eye below (what YouTube's 360 3D expects).
- **Size.** By default each eye's dome gets the game's own pixel density at the image centre, limited to an 8192 frame:
  ETS2 / ATS on a Quest 3 (3072 x 3264 per eye) would need about 4750 px per eye for VR180, so it is capped at 4096 (frame
  8192 x 4096); VR360 is capped at 8192 x 8192. `dome_width` sets the per-eye width yourself. The log line
  `recorder: dome vr180: source density ... -> ... px per eye, clamped to ...` shows what was chosen.
- **What it ignores.** `symmetric_fov`, `head_smoothing` and its settings are not used in dome mode (not needed: the dome
  is world-locked). `swap_eyes` still works.
- **Which games suit it.** Seated games where the world around you matters and you mostly look forward: driving and flight
  sims, cockpit games (ETS2 / ATS fit well). Fast games where you whip your head around show a lot of half-faded
  dome; `fill = black` shows only the live view.
- **Limits.** The video only contains what you looked at. Rotation only: if you lean far forward the painted areas from
  before do not line up perfectly with the live view (small seams). The horizon is levelled from the play space
  (LOCAL / STAGE), and the dome faces where you looked at the first frame of the recording (`dome_forward = start`).
  VR360 frames are big (8192 x 8192 HEVC at most); the dome needs extra VRAM (the output plus two colour and two time
  copies of the dome: ~640 MB at 8192 x 4096, ~1.3 GB at 8192 x 8192; `fill = black` needs only the output).
- **Files and upload.** Files are named `<game>_<time>_VR180.mp4` / `_VR360.mp4`, with a `<same name>.json` next to it
  that tells the tray app what it is. The MP4 carries the Spherical Video V2 boxes (`st3d` + `sv3d`, equirectangular,
  VR180 = left / right cropped to +-90 degrees), and the tray's MKV keeps them (ffmpeg turns `sv3d` into the Matroska
  `Projection` element), so the routine is the same as for flat recordings: **upload the `.mkv`** (balloon "Ready for
  YouTube (VR180): ..."). The MKV route is verified with ffprobe on both files (`tests\run_dome.ps1`); YouTube's own
  processing of these files has not been tried yet -- if YouTube shows it flat, upload the MP4 (`keep_mp4 = 1`) instead,
  which carries the same metadata in the form Google's spatial-media tools write. `post_flat = 1` makes the `_FLAT.mp4`
  from the left eye's view straight ahead (ffmpeg `v360`, 100 degrees wide, 3840 x 2160); `scripts\make_flat.ps1` is
  for side-by-side recordings only.

| Key (`[dome]`) | Default | Meaning |
|---|---|---|
| `projection` | `flat` | `flat` = side-by-side 3D (as before), `vr180`, `vr360`. |
| `dome_width` | `0` | Per-eye width in pixels; `0` = from the game's pixel density (capped at an 8192 frame). |
| `dome_forward` | `start` | The dome faces your head's direction at the start of the recording (only value for now). |
| `fill` | `last_seen` | `last_seen` = keep what you looked at earlier; `black` = only the live view, the rest black. |
| `fill_fade_s` | `8` | Seconds a painted area stays before fading to black (over 2 s); `0` = never fade. |
| `window_feather_deg` | `1` | Soft edge (degrees) where the live view blends into the dome; `0` = hard edge. |

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
Head-motion smoothing and `symmetric_fov` (on by default) add one more RGBA frame of VRAM and one full-screen pass on
the encoder's device: measured
0.2 ms GPU per frame at 12240 x 6496 (ATS on VDXR, encoded at 8192 x 4346) on an RTX 5090, and nothing on the game's
thread or GPU queue.

## With other mods

- **ETS2 / ATS DLAA + DLSS injector**: works alongside; this layer never hooks Present or the game's D3D calls. The
  default hotkey is Ctrl+**Shift**+F10 because plain Ctrl+F10 is the injector's snapshot key.
- **Virtual Desktop's own OpenXR layer** and other implicit layers coexist (each registers its own entry).

## Building from source

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md). Short version: Visual Studio 2022, then
`powershell -ExecutionPolicy Bypass -File scripts\package.ps1`.
