// indicator -- the in-headset recording indicator: a small dot shown to the player only, as an extra OpenXR quad
// composition layer that the layer's xrEndFrame hook appends AFTER the capture (so it is never in the recording).
//
//   IndicatorTracker   recorder state -> what to show (pure logic, no graphics; unit-tested)
//   RenderIndicator    the 64x64 straight-alpha picture for a state / time (pure CPU; unit-tested)
//   VrIndicator        owns a 64x64 swapchain + a VIEW reference space created through the NEXT layer's functions
//                      (the runtime sees a normal app swapchain; our own hooks never see it). D3D11: the image is filled
//                      with UpdateSubresource on the game's immediate context, only on frames where the dot is shown.
//                      D3D12 / Vulkan: not implemented yet (logged once, the indicator stays off; capture unaffected).
// Visual states: starting / finalizing = amber, steady; recording = red, pulsing (alpha 0.35 .. 1.0, 1 Hz);
// saved = green for 1.5 s; failed = red X for 2 s; otherwise hidden (nothing is done at all).
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include "config.h"
#include "frame_source.h"
#include "recorder.h"

enum class IndShow { Hidden, Starting, Recording, Saved, Failed };
const char* IndShowName(IndShow s);

class IndicatorTracker {
public:
    // Per frame. `message` is only read on a transition to Idle / Failed (pass a callback-free string: the caller
    // fetches Recorder::Status().message only when NeedsMessage() says so). now = seconds, any monotonic clock.
    bool NeedsMessage(RecState st) const;
    IndShow Update(RecState st, const std::string& message, double now);
private:
    RecState prev_ = RecState::Idle;
    IndShow timed_ = IndShow::Hidden;
    double until_ = 0;
};

const int kIndicatorPx = 64;
// Fills out[64*64*4] (row-major, top row first) with straight (non-premultiplied) alpha. srgbTexture: the texture is
// an *_SRGB format (bytes = sRGB-encoded colour); else the colour is linearised. bgra: B,G,R,A byte order.
void RenderIndicator(IndShow show, double now, float opacity, bool srgbTexture, bool bgra, uint8_t* out);

// Next-layer entry points the indicator needs (from the instance).
struct IndicatorDispatch {
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
};

class VrIndicator {
public:
    VrIndicator(const IndicatorSettings& s, GfxApi api, ID3D11Device* dev11, bool gameUsesSrgb)
        : s_(s), api_(api), dev11_(dev11), preferSrgb_(gameUsesSrgb) {}
    // Game thread, inside xrEndFrame. Creates the swapchain / space on first use, uploads the picture for `show` and
    // fills `quad`. Returns false (nothing to append) when hidden, disabled or on any failure (then disabled for good).
    bool Frame(XrSession session, const IndicatorDispatch& d, IndShow show, double now, XrCompositionLayerQuad& quad);
    // Before the session is destroyed (or when disabling): destroy our swapchain and space through the next layer.
    void Destroy(const IndicatorDispatch& d);
    bool Disabled() const { return disabled_; }
    void Disable(const IndicatorDispatch& d, const char* why);
    bool Created() const { return swapchain_ != XR_NULL_HANDLE; }

private:
    bool Create(XrSession session, const IndicatorDispatch& d);
    IndicatorSettings s_;
    GfxApi api_;
    Microsoft::WRL::ComPtr<ID3D11Device> dev11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx11_;
    bool preferSrgb_ = true;
    bool disabled_ = false, triedCreate_ = false;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images_;
    bool srgb_ = true, bgra_ = false;
    std::vector<uint8_t> pixels_;
};
