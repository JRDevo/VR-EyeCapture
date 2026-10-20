# Vulkan support — implementation plan (2026-10-20)

Written by a read-only Fable pass over the code; spot-checked by the lead session (`src/source_vulkan.cpp` stub,
`src/layer.cpp:1053/1110`). Not implemented yet. Older notes: [`VULKAN.md`](VULKAN.md). Line numbers are from
master at fe03c7e.

Dev box: RTX 5090, driver 617.42, `C:\Windows\System32\vulkan-1.dll` present, no LunarG SDK. The mock-based tests
can run against a real Vulkan driver here, so the D3D11 <-> Vulkan interop is testable without a headset.

## 0. Where the code stands

- `src/source_vulkan.cpp:1-9`: `CreateVulkanFrameSource()` logs "not yet supported", returns nullptr. No arguments (`src/frame_source.h:463`).
- `src/layer.cpp:1110-1115`: `XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR` (== `..._VULKAN2_KHR`) recognised in `Hook_CreateSession`; binding fields not read.
- `src/layer.cpp:1053-1057`: `Hook_EnumerateSwapchainImages` already stores `VkImage` handles as `void*`.
- `src/layer.cpp:958`: `SwapchainInfo::format` is raw, so a `VkFormat` for Vulkan — but `layer.cpp:484` (`OverlayFamily`), `:569` (`IsSrgbDxgi`), `:799` (indicator srgb) cast it to `DXGI_FORMAT`. Latent bugs; phase 0 fixes them.
- `src/layer.cpp:1126`, `src/live_share.cpp:294-298, :843`: live share D3D11/D3D12 only.
- `src/indicator.cpp:100-104`: `VrIndicator::Create` D3D11 only.
- `src/openvr/openvr_capture.cpp:412-421`: `OnSubmitImpl` rejects every `eType != TextureType_DirectX`. `third_party/openvr/openvr.h` has `TextureType_Vulkan = 2` and `Submit_VulkanTextureWithArrayData = 0x40` but not `VRVulkanTextureData_t`.
- The encoder contract is API-independent: `Recorder::Impl::Init` uses `src->AdapterLuid()` (`recorder.cpp:838-841`) and `src->CreateRing(...)` (`:960`), ring = `SharedRing{ID3D11Texture2D[], ID3D11Fence ready/free}` (`frame_source.h:421-427`). Reproject, smoothing, dome, overlays, NV12, MF do not know the game API.

## 1. OpenXR Vulkan games (XR_KHR_vulkan_enable / enable2)

Hooks (add to `kHooks`, `layer.cpp:1303-1308`; `RESOLVE` at `:1361-1366`; `Layer_GetInstanceProcAddr` only substitutes when next returned a function, so this is safe on runtimes without the extension):

| Hook | Why |
|---|---|
| `xrCreateVulkanInstanceKHR` (enable2) | Deep-copy create info; below API 1.1 append `VK_KHR_get_physical_device_properties2`, `VK_KHR_external_memory_capabilities`, `VK_KHR_external_semaphore_capabilities`. Capture `pfnGetInstanceProcAddr` and the `VkInstance`. |
| `xrCreateVulkanDeviceKHR` (enable2) | Deep-copy `VkDeviceCreateInfo`; append `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32` (+ 1.0 prerequisites), dedup; enable `timelineSemaphore` via `VkPhysicalDeviceVulkan12Features` if present, else `VkPhysicalDeviceTimelineSemaphoreFeatures` (never both). Record final extension list per `VkDevice`. Only place to inject on enable2. |
| `xrGetVulkanInstanceExtensionsKHR`, `xrGetVulkanDeviceExtensionsKHR` (enable v1) | Two-call idiom; append missing names to the space-separated string. No feature injection possible here. |
| `xrGetVulkanGraphicsDevice(2)KHR` | Optional: log the physical device. |

`xrCreateSession`: read `XrGraphicsBindingVulkanKHR{instance, physicalDevice, device, queueFamilyIndex, queueIndex}` into a new `VulkanBinding` (in `frame_source.h`), plus `pfnGetInstanceProcAddr` (else `GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkGetInstanceProcAddr")`; never link `vulkan-1.lib`). `VkQueue` via `vkGetDeviceQueue`.

VkImage per eye: nothing new — `CaptureImpl` (`layer.cpp:683-712`) already fills `EyeView` from acquire/release tracking.

Layout and sync in `VulkanSource::CopyEyes` (game thread, before the runtime's `xrEndFrame`):
- Released colour images are `COLOR_ATTACHMENT_OPTIMAL` (UAV-only: `GENERAL` — verify in the spec). Barrier to `TRANSFER_SRC_OPTIMAL`, copy, barrier back. Subresource layer = `arrayIndex`.
- MSAA: `vkCmdResolveImage` into a cached single-sample image per (swapchain, eye), like `D3D12Source::ResolveTarget` (`source_d3d12.cpp:334-356`).
- Ring image: `UNDEFINED`/`GENERAL -> TRANSFER_DST_OPTIMAL` with acquire from `VK_QUEUE_FAMILY_EXTERNAL`, copy, `-> GENERAL` with release to external.
- One command buffer + `VkFence` per ring slot; fence not signalled -> drop the frame (as `source_d3d12.cpp:95`). Allocate all in `CreateRing`.
- `vkQueueSubmit` on the binding queue from the xrEndFrame thread (spec says the app must not use that queue concurrently with runtime calls — quote it in the source).
- `DestroyRing`: `vkWaitForFences` (2 s cap), then destroy.

## 2. OpenVR Vulkan games

- Add Valve's `VRVulkanTextureData_t` and `VRVulkanTextureArrayData_t` to `third_party/openvr/openvr.h` (BSD-3, covered by THIRD_PARTY).
- `OnSubmitImpl`: branch on `eType`; for `TextureType_Vulkan`, `handle` points to the struct (not a VkImage). Vulkan twins of `EnsureSource` / `LookupTexture` keyed by `m_nImage`; array index from `Submit_VulkanTextureWithArrayData`.
- Bounds, both-eyes-in-one-texture, arrays: unchanged logic. sRGB from `DxgiFromVkFormat(m_nFormat)`.
- Valve's contract: the submitted image is in `TRANSFER_SRC_OPTIMAL` (verify in `openvr.h`) -> no source layout change, only a barrier. Make the source layout an input to the shared `CopyEyes`.
- Hook compositor `GetVulkanInstanceExtensionsRequired` / `GetVulkanDeviceExtensionsRequired` per version (`_022`: slots 39/40; `_028`/`_029` likely shifted — verify against `openvr_capi.h`). Probably a safety net: SteamVR already needs external memory. Log the list the game gets.
- Quick mode untouched.

## 3. Frames to the encoder — recommendation: option A

**A (recommended).** Ring textures created on the encoder D3D11 device as today (`source_d3d11.cpp:58-69`, NT handles), imported into Vulkan: `VkExternalMemoryImageCreateInfo{D3D11_TEXTURE_BIT}`, dedicated allocation with `VkImportMemoryWin32HandleInfoKHR`, then `CloseHandle`. Check importability with `vkGetPhysicalDeviceImageFormatProperties2`. Fences: `ID3D11Device5::CreateFence(SHARED)` on the encoder device, imported with `vkImportSemaphoreWin32HandleKHR{D3D12_FENCE_BIT}`. Overlay atlases same path. Recorder, LiveShare, OBS consumer unchanged; zero CPU copies.
- Semaphores: baseline binary semaphore + `VkD3D12FenceSubmitInfoKHR` (no timeline feature needed; works on v1 and OpenVR). Timeline semaphores when known enabled. **Confirm `VkD3D12FenceSubmitInfoKHR` semantics in the spec before coding.**

**B** Vulkan-exported memory opened in D3D11: not portable. Rejected.
**C** CPU readback: ~5 GB/s at Quest 3 res. Not planned.

Missing extensions (v1 path / OpenVR): probe `vkGetDeviceProcAddr` for `vkImportSemaphoreWin32HandleKHR`; first import failure is final. Then log "capture disabled (the game runs normally)" and return nullptr. New config `vulkan_inject_extensions = 1`; test env `XR_EYE_CAPTURE_TEST_VK_NO_INJECT=1`.

Adapter LUID: `VkPhysicalDeviceIDProperties::deviceLUID` (`deviceLUIDValid` required).

Format map `DxgiFromVkFormat`: `R8G8B8A8_UNORM/SRGB -> R8G8B8A8_UNORM`, `B8G8R8A8_UNORM/SRGB -> B8G8R8A8_UNORM`, `A2B10G10R10_UNORM_PACK32 -> R10G10B10A2_UNORM`, `R16G16B16A16_SFLOAT -> R16G16B16A16_FLOAT`, `B10G11R11_UFLOAT_PACK32 -> R11G11B10_FLOAT` (overlays only).

## 4. Per feature

- Head smoothing, symmetric_fov, dome, YouTube layout, overlay compositor: no change.
- record_overlays: add `SwapchainInfo::dxgi`, fix the three casts; copy overlay images with the same barriers.
- Indicator (OpenXR): Vulkan branch in `VrIndicator::Create`; per frame `vkCmdCopyBufferToImage` from a mapped 16 KB buffer per swapchain image. Shared helper `src/vk_device.{h,cpp}` (function loader, memory type pick, LUID).
- Indicator (OpenVR): `IVROverlay::SetOverlayRaw` (CPU bytes, no GPU device needed); slot per version.
- OBS live share: `LiveShareParams` gets a source factory instead of D3D fields; gates at `layer.cpp:1126`, `live_share.cpp:843` admit Vulkan. Second source on the same queue, same thread -> ordering only.
- Config reload, hotkey, tray, MKV: nothing.

## 5. Tests

- Vulkan-Headers via FetchContent (header only, `VK_NO_PROTOTYPES`); tests load `vulkan-1.dll` dynamically.
- `tests/mock_runtime.cpp`: advertise enable2 + v1, implement the Vulkan functions, Vulkan swapchains; print the enabled device extensions (proves injection reached the runtime).
- `tests/layer_e2e.cpp --api vulkan [--vk1] [--vk-no-ext]`: device made WITHOUT external-memory extensions; clear-colour test pattern; `verify_mp4.ps1` pixel checks; `--vk-no-ext` checks the graceful disable. Run once under `VK_LAYER_KHRONOS_validation` (needs LunarG SDK on the dev box).
- `tests/vulkan_test.cpp` (CPU, CI): format map, string append, create-info injection, OpenVR struct -> `EyeView`.
- `tests/mock_openvr.cpp` + `openvr_e2e.cpp --api vulkan`: two / wide (bounds) / array / msaa; extension-list check.
- Real titles: `hello_xr -g Vulkan2`, a Godot 4 XR demo, a UE5 VR template with the Vulkan RHI, X-Plane 12 (OpenXR); DOOM VFR, Wolfenstein: Cyberpilot, No Man's Sky (OpenVR; NMS Vulkan submit not certain).

## 6. Phases

| Phase | Content | Size | Risks |
|---|---|---|---|
| 0 Groundwork | headers, `vk_device`, format map, `SwapchainInfo::dxgi` + cast fixes, `VulkanBinding`, `vulkan_test` | ~350 lines | none |
| 1 OpenXR capture | `source_vulkan.cpp`, hooks, mock runtime, e2e cases | ~1000 lines | fence-import semantics, UAV layout, queue contract |
| 2 Live share + indicator + overlays | | ~450 lines | second source on one queue |
| 3 OpenVR | structs, submit branch, extension slots, `SetOverlayRaw`, mock cases | ~550 lines | slot numbers per version, layout contract |
| 4 Hardware + docs | real titles; update `VULKAN.md`, README, DEVELOPMENT | docs | — |

Unverified without hardware: what VDXR / SteamVR-OpenXR do with the appended extensions and released layouts; fence import on AMD/Intel; engine threading on the binding queue (UE RHI thread); OpenVR `_028`/`_029` slots and SteamVR's extension list; per-frame cost of the extra barriers.

Before phase 1: confirm `VkD3D12FenceSubmitInfoKHR` with binary semaphores, and the released-image layout for UAV-only swapchains.
