# Third-party components

- **OpenXR SDK** (Khronos Group), <https://github.com/KhronosGroup/OpenXR-SDK>, tag `release-1.1.62`, fetched by CMake at
  configure time. License: Apache-2.0 (headers: Apache-2.0 OR MIT). The layer DLL uses only the headers. The OpenXR
  loader (`openxr_loader.dll`) is built for the test programs and is not part of the release zip.
- **OpenVR SDK header** (Valve Corporation), <https://github.com/ValveSoftware/openvr>, `headers/openvr.h`. License:
  BSD-3-Clause (the notice is in the file). `third_party/openvr/openvr.h` is a minimal hand-transcribed subset (types,
  enums and interface method orders only) used to build the SteamVR proxy `steamvr\openvr_api.dll`; no Valve code or
  library is linked. The proxy loads the game's own Valve `openvr_api.dll` (renamed `openvr_api_real.dll`) at run time.
- **OpenVR API library** (Valve Corporation), `openvr_api.dll` v2.15.6 for Windows x64 from the OpenVR SDK release
  (`bin/win64`), Valve-signed, unmodified, SHA-256 `BAB8AC6EF64E68A9CA53315B0014D131088584B2EFDFA6DB511D67EC03CFCB4A`.
  License: BSD-3-Clause (`third_party/openvr/LICENSE`, shipped as `valve\LICENSE.txt`). Shipped in the release zip as
  `valve\openvr_api.dll`; only `EyeCaptureMirror.exe` (SteamVR quick mode) loads it, to read SteamVR's mirror texture.
- **Windows SDK components** used through the operating system (not redistributed): Direct3D 11 / 12, DXGI,
  Media Foundation (sink writer, hardware encoder MFTs, AAC encoder), WASAPI, D3DCompiler.
