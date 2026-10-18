# Third-party components

- **OpenXR SDK** (Khronos Group), <https://github.com/KhronosGroup/OpenXR-SDK>, tag `release-1.1.62`, fetched by CMake at
  configure time. License: Apache-2.0 (headers: Apache-2.0 OR MIT). The layer DLL uses only the headers. The OpenXR
  loader (`openxr_loader.dll`) is built for the test programs and is not part of the installer.
- **OpenVR SDK header** (Valve Corporation), <https://github.com/ValveSoftware/openvr>, `headers/openvr.h`. License:
  BSD-3-Clause (the notice below; it is also at the top of `third_party/openvr/openvr.h`). `third_party/openvr/openvr.h` is
  a hand-written minimal subset DERIVED from Valve's header (types, enums, interface method orders and the function-table
  layouts only), compiled into the SteamVR proxy `steamvr\openvr_api.dll` and `EyeCaptureMirror.exe`. No Valve code or
  library is linked into or shipped with VR EyeCapture: the proxy loads the game's own Valve `openvr_api.dll` (renamed
  `openvr_api_real.dll`) at run time, and the SteamVR quick mode (`EyeCaptureMirror.exe`) loads SteamVR's own copy from the
  user's SteamVR installation (`<runtime>\bin\win64\openvr_api.dll`, found through
  `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`). The tests use that same SteamVR copy when it is installed (and skip
  otherwise); the repository holds no Valve binary.

  ```
  Copyright (c) 2015, Valve Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
  following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
     disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
     disclaimer in the documentation and/or other materials provided with the distribution.
  3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
     products derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
  INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  ```
- **OBS Studio / libobs headers** (OBS Project), <https://github.com/obsproject/obs-studio>, tag `31.0.0`, only the
  `libobs/` headers, fetched by CMake at configure time when `EYE_BUILD_OBS_PLUGIN=ON` (shallow clone, no submodules; OBS
  is not built). License: GPL-2.0-or-later. Used only by the OBS plugin `obs-eyecapture.dll` (the separate
  `VR-EyeCapture-OBS-v<version>.zip`), which is itself GPL-2.0-or-later (`obs/COPYING.md`). The import library for
  `obs.dll` is generated from `obs/obs.def` (export names only). OBS Studio itself is not redistributed; the plugin runs
  inside the user's own OBS installation.
- **Windows SDK components** used through the operating system (not redistributed): Direct3D 11 / 12, DXGI,
  Media Foundation (sink writer, hardware encoder MFTs, AAC encoder), WASAPI, D3DCompiler.
- **ffmpeg** is not shipped: you install it yourself (`winget install Gyan.FFmpeg.Essentials`) and the tray app runs it
  as a separate program for the MKV / 8K / flat conversions.
