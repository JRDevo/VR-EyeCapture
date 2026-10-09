# Third-party components

- **OpenXR SDK** (Khronos Group), <https://github.com/KhronosGroup/OpenXR-SDK>, tag `release-1.1.62`, fetched by CMake at
  configure time. License: Apache-2.0 (headers: Apache-2.0 OR MIT). The layer DLL uses only the headers. The OpenXR
  loader (`openxr_loader.dll`) is built for the test programs and is not part of the release zip.
- **Windows SDK components** used through the operating system (not redistributed): Direct3D 11 / 12, DXGI,
  Media Foundation (sink writer, hardware encoder MFTs, AAC encoder), WASAPI, D3DCompiler.
