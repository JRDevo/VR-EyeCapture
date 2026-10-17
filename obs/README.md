# obs-eyecapture -- OBS Studio source for VR EyeCapture

An OBS Studio source, **VR EyeCapture** (id `eyecapture_source`; shown as **OpenXR EyeCapture** before the rename, and saved
scenes keep working because the id did not change), that shows the live eye images of a running VR game recorded by
VR EyeCapture (the OpenXR layer or the SteamVR proxy). No window capture, no mirror window: the layer hands the eye
images to OBS through shared GPU textures (protocol and threading: `docs/DEVELOPMENT.md`, "Live share (OBS)").

- **View**: Left eye (default) / Right eye / Side by side.
- **Crop to 16:9 (center)**: on by default, for the left / right eye views (VR eyes are taller than 16:9).
- **Max fps**: 60 by default; the game copies at most this many frames per second for OBS.
- **Re-projection**: **Same as recording (config.ini)** (default) = the recorder's symmetric fov / head smoothing with
  the game's `config.ini` values (`symmetric_fov`, `head_smoothing`, `head_follow_deg`, `roll_lock`, `output_fov_scale`,
  `edge_guard`) and, with `record_overlays = 1`, the game's overlay layers composited like a recording, rendered by the
  layer on its own GPU device (never on OBS's); **Raw eyes (off)** = the images as the game rendered them (no overlays).
  A change rebuilds the game's share ring (the picture is back about a second later). All EyeCapture sources of one OBS
  share one ring: the re-projection when any visible source wants it.
- **Size**: **Full** (default) / **Half** = the layer's pass renders the ring at half width and height (2 x 2 taps per
  pixel, no aliasing; raw eyes through an identity pass). Less VRAM and a quarter of the copy into OBS; the full-size
  copy on the game's GPU stays the same. Shared ring: Full when any visible source wants it.
- **Zoom**: 100 .. 250 % (default 100), Left / Right eye views: a centred crop of the eye (before the 16:9 crop) drawn
  scaled up to the source's size (`gs_draw_sprite_subregion` under a scale matrix). Plugin only; the game is not told.

The game pays only while the source is **showing** (in the preview or on air): two extra copies on its GPU per shared
frame (+ the overlay sub-images when it shows overlays; + one pass on the layer's own device for the re-projection,
overlays or Half). With no showing source the layer only checks once per second whether one appeared. `live_share = 0`
in `config.ini` switches the layer side off. Not in the live view: dome mode (VR180 / VR360; the live view stays flat)
and the in-headset indicator (which shows a violet dot while OBS receives frames and nothing is recorded,
`indicator_live`). Overlays: the first overlay layer of a session restarts the ring once (about a second without picture).

The plugin and the layer speak protocol version 3 (`src/live_share_proto.h`); a plugin and a layer of different versions
ignore each other (logged), so update both together.

Requirements: OBS on the **Direct3D 11** renderer (the Windows default; with OpenGL the source logs a warning and stays
empty), OBS and the game on the **same GPU**, and OBS not running with fewer rights than the game (the plugin opens the
game process with `PROCESS_DUP_HANDLE`): OBS as administrator with a normal game works (the plugin creates its shared
block so that a normal game may open it), but an elevated game and a normal OBS will not (logged by the plugin:
`OpenProcess(PROCESS_DUP_HANDLE) on the game ... failed`). An older plugin run as administrator hides the block from a
normal game; the layer then logs `live share: block ... exists, but this game may not open it (access denied)` once.
One OBS process at a time uses the live view; all its EyeCapture sources share it.

## Build

Visual Studio 2022 + its bundled CMake, from the repo root (the same build folder as the layer):

```
cmake -S . -B build -DEYE_BUILD_OBS_PLUGIN=ON
cmake --build build --config Release --target obs-eyecapture
```

The configure step fetches only the headers of OBS Studio 31.0.0 (`libobs/`; shallow clone, no submodules, OBS is not
built). The import library for `obs.dll` is generated at build time from `obs/obs.def` (`lib /def:obs.def /machine:x64`),
which lists only the exports the plugin uses -- no OBS binaries are needed to build. When the plugin starts using another
libobs function, add its name to `obs.def` (check it with `dumpbin /exports "C:\Program Files\obs-studio\bin\64bit\obs.dll"`).
The libobs public headers need none of OBS's configure-time generated files.

Output: `build\Release\obs-eyecapture.dll`.

## Install

Copy the DLL to

```
C:\ProgramData\obs-studio\plugins\obs-eyecapture\bin\64bit\obs-eyecapture.dll
```

(create the folders) and restart OBS. Add a source: **+** > **VR EyeCapture**. It is empty (0 x 0) until a game
with the layer runs; the game's log (`logs\eye_capture.log`) then says `live share: consumer found ...` and
`live share: ring ready ...`, and OBS's log has `[obs-eyecapture] producer '<game>' ... imported`.

## Test without OBS

`tests\run_live.ps1` runs the plugin's consumer code (`obs/live_consumer.cpp`, through `tests/live_share_test.cpp`)
against the layer and the mock runtime and checks the pixels, the frame rate throttle, the slot hand-off, the
re-projection (symmetric size, head motion, the switch to raw eyes), Half size, overlays in the live frame, a recording
at the same time and the teardown. The OBS-side drawing (`obs/plugin.cpp`: texture, crop, zoom, sRGB) is only checked in
OBS itself.
