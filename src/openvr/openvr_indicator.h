// openvr_indicator -- the in-headset recording dot for SteamVR games (the OpenXR layer's VrIndicator counterpart).
//
// An IVROverlay created by the proxy inside the game process, attached to the HMD at indicator_x / _y / _z (metres, -Z =
// ahead), indicator_size wide. SteamVR's compositor draws it on top of the game's frame, AFTER the proxy copied the game's
// eye textures in Submit -- so it is never in the recording. Same states and colours as the OpenXR dot (IndicatorTracker /
// RenderIndicator, src/indicator.h): amber while starting / finalizing, red pulsing (1 Hz, through SetOverlayAlpha, no
// texture upload) while recording, green 1.5 s after a save, red X 2 s after a failure, violet while a live viewer receives
// frames (indicator_live), hidden otherwise.
//
// Called on the game's render thread inside the second eye's Submit (under the proxy's lock), on the game's D3D11 device:
// the 64x64 texture is filled with UpdateSubresource on the game's immediate context, only when the shown state changes.
// The overlay is created on the first frame that shows something. Any failure: logged once, the dot is off for this VR
// session, recording is not affected. Quick mode (EyeCaptureMirror) has no dot: it records the compositor output, a dot
// there would be in the video (user decision 2026-10-18).
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include "../../third_party/openvr/openvr.h"
#include "config.h"
#include "indicator.h"
#include "openvr_versions.h"

class OvrIndicator {
public:
    explicit OvrIndicator(const IndicatorSettings& s) : s_(s) {}
    // Per frame pair. get = the real DLL's VR_GetGenericInterface; dev = the game's device. Never throws; a structured
    // exception inside the runtime's overlay calls disables the dot (logged) instead of reaching the capture.
    void Frame(PFN_VR_GetGenericInterface get, ID3D11Device* dev, IndShow show, double now);
    // Before VR_ShutdownInternal: destroys the overlay (the runtime would drop it with the connection anyway).
    void Destroy();
    bool Disabled() const { return disabled_; }

private:
    bool FrameImpl(PFN_VR_GetGenericInterface get, ID3D11Device* dev, IndShow show, double now);
    bool Create(PFN_VR_GetGenericInterface get, ID3D11Device* dev);
    void Disable(const char* why, int err);

    IndicatorSettings s_;
    const OverlayVersion* ver_ = nullptr;
    void* fn_[kFnTableMax] = {};
    uint64_t handle_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    bool tried_ = false, disabled_ = false, visible_ = false, loggedShown_ = false;
    IndShow uploaded_ = IndShow::Hidden;
    float alpha_ = -1.f;
    double lastAlphaAt_ = -1.0;
};
