# Vulkan capture — phase 2 plan

Status: **not implemented**. A Vulkan session logs `Vulkan not yet supported (phase 2, see docs/VULKAN.md)` and passes
through (`src/source_vulkan.cpp`, `CreateVulkanFrameSource`). The layer already recognises
`XrGraphicsBindingVulkanKHR` / `XrGraphicsBindingVulkan2KHR` (same structure type value) and records `VkImage` handles
from `XrSwapchainImageVulkanKHR` in `xrEnumerateSwapchainImages`, so only the `IFrameSource` implementation is missing.

## Shape (same as D3D11 / D3D12, see `src/frame_source.h`)

The encoder stays on the private D3D11 device. A Vulkan frame source must provide:

1. **Shared SBS ring.** Create each ring texture on the encoder D3D11 device (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
   SHARED`), export an NT handle, and import it into the game's `VkDevice`:
   `VkExternalMemoryImageCreateInfo{ handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT }` on the image,
   `VkImportMemoryWin32HandleInfoKHR` + `VkMemoryDedicatedAllocateInfo` on the allocation
   (the reverse direction — a Vulkan-exported `OPAQUE_WIN32` image opened with `OpenSharedResource1` — is not portable
   across vendors). Format: the `VkFormat` → `DXGI_FORMAT` map of the copy family (`VK_FORMAT_R8G8B8A8_SRGB` →
   `R8G8B8A8_UNORM` + sRGB-encoded, `B8G8R8A8_SRGB` → `B8G8R8A8_UNORM`, ...).
2. **Shared fences.** Create the two fences on the encoder device (`ID3D11Device5::CreateFence(D3D11_FENCE_FLAG_SHARED)`),
   export, and import as **timeline semaphores** in Vulkan with
   `VkImportSemaphoreWin32HandleInfoKHR{ handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT }`.
   The game queue waits on `free` and signals `ready` through `VkTimelineSemaphoreSubmitInfo`.
3. **Copy on the game's queue in xrEndFrame.** One command buffer per ring slot from our own command pool on
   `XrGraphicsBindingVulkanKHR::queueFamilyIndex`: barrier the swapchain image
   `COLOR_ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL` (the layout a released colour swapchain image is in, per
   `XR_KHR_vulkan_enable`), `vkCmdCopyImage` (or `vkCmdResolveImage` for MSAA) into the ring image at x = 0 / leftWidth
   with `srcSubresource.baseArrayLayer = imageArrayIndex`, barrier back, ring image to `GENERAL` with a queue-family
   release to `VK_QUEUE_FAMILY_EXTERNAL`. `vkQueueSubmit` on the binding's queue (`queueIndex`) from the xrEndFrame
   thread — the app must not use that queue concurrently, which it may not during xrEndFrame anyway.
   Function pointers come from `vkGetDeviceProcAddr` obtained through `XrGraphicsBindingVulkanKHR::instance` and the
   process's `vulkan-1.dll`.

## The hard part: device extensions

Steps 1–2 need the game's `VkDevice` to have enabled `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`,
`VK_KHR_timeline_semaphore` (core in 1.2) and `VK_KHR_dedicated_allocation` (core in 1.1). A game will not enable those
on its own, so the layer must add them where OpenXR lets it:

- **`XR_KHR_vulkan_enable2`**: hook `xrCreateVulkanDeviceKHR` and append the extensions (and the
  `timelineSemaphore` feature) to `vulkanCreateInfo->ppEnabledExtensionNames` before forwarding.
- **`XR_KHR_vulkan_enable`** (v1): hook `xrGetVulkanDeviceExtensionsKHR` and append the names to the space-separated
  list the runtime returns (the app passes that list to `vkCreateDevice`).
- If neither path lets us add them (the app ignores the runtime's list), log "Vulkan device lacks external memory —
  capture disabled" and pass through.

A fallback without external memory (copy to a host-visible buffer, CPU upload to D3D11) is possible but would cost
~80 MB of PCIe traffic per frame at Quest 3 resolution; not planned.

## Test plan

Extend `tests/mock_runtime.cpp` with `XR_KHR_vulkan_enable2` (create the VkDevice it is asked for, `VkImage` swapchains)
and `tests/layer_e2e.cpp` with `--api vulkan` (clear the eye images with `vkCmdClearColorImage` using
`tests/test_pattern.h` rectangles), then reuse `tests/verify_mp4.ps1`. Vulkan headers: FetchContent
`KhronosGroup/Vulkan-Headers` (header-only; no SDK install needed).
