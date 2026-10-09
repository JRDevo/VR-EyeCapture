// frame_source -- the graphics-API side of a capture: copying the two eye images of one OpenXR frame from the
// game's swapchain images into a shared side-by-side texture that the encoder's private D3D11 device can read.
//
// Design (same for every API):
//   * The encoder ALWAYS runs on its own D3D11 device (created by the Recorder on the game's adapter, by LUID).
//     Media Foundation never touches the game's device.
//   * A ring of N shared SBS textures (left eye at x = 0, right eye at x = leftWidth) is visible on both the game
//     device and the encoder device.
//   * Two shared fences order the work across devices without any CPU waits on the game thread:
//       ready: signalled on the game's GPU queue after the two copies into a slot (one value per frame);
//              the encoder GPU-waits for it before reading the slot.
//       free:  signalled by the encoder after it has consumed a slot; the game queue GPU-waits for it before
//              copying into that slot again.
//   * CopyEyes runs on the game thread inside xrEndFrame: a GPU wait, two copies (or resolve + copy for MSAA), a
//     signal and a flush/submit. Nothing blocks the CPU; if a slot's previous copy has not finished on the GPU the
//     frame is dropped (counted) instead of waiting.
#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct ID3D12Device;
struct ID3D12CommandQueue;

enum class GfxApi { None, D3D11, D3D12, Vulkan, OpenGL, Unknown };
const char* GfxApiName(GfxApi api);

// The graphics-API independent description of one OpenXR swapchain (from XrSwapchainCreateInfo).
struct SwapchainInfo {
    int64_t  format = 0;          // DXGI_FORMAT for D3D11/D3D12, VkFormat for Vulkan
    uint32_t width = 0, height = 0, arraySize = 1, mipCount = 1, sampleCount = 1, faceCount = 1;
    uint64_t usageFlags = 0;      // XrSwapchainUsageFlags
};

// One eye of one frame: which image, which array slice, which rectangle.
struct EyeView {
    void*                image = nullptr;   // ID3D11Texture2D* / ID3D12Resource* / VkImage
    const SwapchainInfo* sc = nullptr;
    const void*          swapchainKey = nullptr;  // identity for per-swapchain caches (MSAA resolve targets)
    uint32_t             arrayIndex = 0;
    int32_t              x = 0, y = 0;
    uint32_t             w = 0, h = 0;
    // The view's rendering pose (XrCompositionLayerProjectionView::pose / fov, in the projection layer's space).
    // Only head-motion smoothing reads it; the frame sources ignore it.
    bool                 hasPose = false;
    float                orientation[4] = {0, 0, 0, 1};   // x, y, z, w
    float                position[3] = {0, 0, 0};
    float                fov[4] = {0, 0, 0, 0};           // angleLeft, angleRight, angleUp, angleDown (radians)
};

// The ring as the encoder device sees it.
struct SharedRing {
    std::vector<ComPtr<ID3D11Texture2D>> tex;   // opened / created on the encoder device
    ComPtr<ID3D11Fence> ready;                  // game queue -> encoder
    ComPtr<ID3D11Fence> free;                   // encoder -> game queue
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
};

class IFrameSource {
public:
    virtual ~IFrameSource() {}
    virtual GfxApi Api() const = 0;
    virtual LUID AdapterLuid() const = 0;
    // Typed staging format for the SBS texture (same copy family as the swapchain texture) and whether the data is
    // sRGB-encoded (true for *_SRGB swapchain formats). false = format not supported for capture.
    virtual bool StageFormat(const SwapchainInfo& sc, DXGI_FORMAT* stage, bool* srgbEncoded) const = 0;
    // Encoder worker thread: create `count` shared SBS textures + the two shared fences, all opened on encDev.
    virtual bool CreateRing(ID3D11Device* encDev, UINT w, UINT h, DXGI_FORMAT fmt, UINT count, SharedRing& out) = 0;
    // Game thread (inside xrEndFrame): GPU-wait free >= waitFree, copy L to (0,0) and R to (dstXRight,0) of slot,
    // signal ready. Returns the ready fence value of this frame, or 0 if nothing was submitted (frame dropped).
    // The caller has already clipped the eye rectangles to the slot.
    virtual uint64_t CopyEyes(UINT slot, const EyeView& L, const EyeView& R, UINT dstXRight, uint64_t waitFree) = 0;
    // Encoder worker thread, after the game thread has stopped calling CopyEyes: wait for the last copy, release.
    virtual void DestroyRing() = 0;
    // Game thread: a swapchain was destroyed; drop caches keyed on it.
    virtual void ForgetSwapchain(const void* swapchainKey) = 0;
};

// Factories. They log and return nullptr when the device lacks what capture needs (e.g. shared fences).
std::unique_ptr<IFrameSource> CreateD3D11FrameSource(ID3D11Device* gameDevice);
std::unique_ptr<IFrameSource> CreateD3D12FrameSource(ID3D12Device* device, ID3D12CommandQueue* queue);
// Vulkan: phase 2 (docs/VULKAN.md). Currently logs "Vulkan not yet supported" and returns nullptr.
std::unique_ptr<IFrameSource> CreateVulkanFrameSource();

// Shared helpers (frame_source.cpp).
// Typed copy-compatible staging format for a DXGI swapchain / texture format, or DXGI_FORMAT_UNKNOWN.
DXGI_FORMAT StageFormatForDxgi(DXGI_FORMAT f);
bool        IsSrgbDxgi(DXGI_FORMAT f);
const char* DxgiFormatName(DXGI_FORMAT f);
// Create the private encoder D3D11 device on the adapter with this LUID (video support, multithread protected).
ComPtr<ID3D11Device> CreateEncoderDevice(LUID luid, std::string* adapterName);
int64_t QpcNow100ns();
