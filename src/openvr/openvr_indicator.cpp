// openvr_indicator -- see openvr_indicator.h.
#include "openvr_indicator.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include "log.h"
#include "openvr_hooks.h"

namespace {
// C function tables of IVROverlay (openvr_capi.h): no this pointer; EVROverlayError is an int (0 = none).
typedef int (*PFN_OvCreate)(char* key, char* name, uint64_t* handle);
typedef int (*PFN_OvHandle)(uint64_t handle);                                   // DestroyOverlay / ShowOverlay / HideOverlay
typedef int (*PFN_OvFloat)(uint64_t handle, float v);                           // SetOverlayAlpha / SetOverlayWidthInMeters
typedef int (*PFN_OvUint)(uint64_t handle, uint32_t v);                         // SetOverlaySortOrder
typedef int (*PFN_OvTransform)(uint64_t handle, uint32_t device, vr::HmdMatrix34_t* m);
typedef int (*PFN_OvTexture)(uint64_t handle, vr::Texture_t* tex);

const uint32_t kHmd = 0;            // k_unTrackedDeviceIndex_Hmd
const double kPi = 3.14159265358979323846;

// The pulse of RenderIndicator's recording dot (0.35 .. 1.0 at 1 Hz), applied through SetOverlayAlpha.
float PulseAlpha(double now) { return (float)(0.675 + 0.325 * std::sin(2 * kPi * now)); }
}

void OvrIndicator::Disable(const char* why, int err) {
    if (disabled_) return;
    disabled_ = true;
    if (err) LOGW("indicator: %s (IVROverlay error %d) -- no dot in the headset for this VR session (recording unaffected)", why, err);
    else LOGW("indicator: %s -- no dot in the headset for this VR session (recording unaffected)", why);
    if (handle_ && ver_) {
        __try { ((PFN_OvHandle)fn_[ver_->destroy])(handle_); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    handle_ = 0;
}

bool OvrIndicator::Create(PFN_VR_GetGenericInterface get, ID3D11Device* dev) {
    tried_ = true;
    if (!get || !dev) { Disable("no VR_GetGenericInterface / D3D11 device", 0); return false; }
    void* table = nullptr;
    for (int i = 0; i < kOverlayVersionCount && !table; ++i) {
        const std::string name = std::string(kFnTablePrefix) + kOverlayVersions[i].name;
        vr::EVRInitError e = vr::VRInitError_None;
        void* t = get(name.c_str(), &e);
        if (t && e == vr::VRInitError_None) { table = t; ver_ = &kOverlayVersions[i]; }
    }
    if (!table) { Disable("the runtime offers none of IVROverlay_027 / _026 / _025", 0); return false; }
    const uint32_t need = ver_->setTexture + 1;
    if (CopyFnTable(static_cast<void* const*>(table), need, fn_) < need) { Disable("IVROverlay function table unreadable", 0); return false; }
    for (uint32_t i = 0; i < need; ++i)
        if (!fn_[i]) { Disable("IVROverlay function table has empty entries", 0); return false; }

    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = kIndicatorPx;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &tex_);
    if (FAILED(hr)) {
        LOGW("indicator: CreateTexture2D(64x64) failed hr=0x%08lx", (unsigned long)hr);
        Disable("no texture for the dot", 0);
        return false;
    }
    dev->GetImmediateContext(&ctx_);

    // the key must be unique system-wide (another game with the proxy may run): one per process
    char key[64], name[] = "VR EyeCapture recording";
    snprintf(key, sizeof key, "jrdevo.eyecapture.indicator.%lu", (unsigned long)GetCurrentProcessId());
    int err = ((PFN_OvCreate)fn_[ver_->create])(key, name, &handle_);
    if (err || !handle_) { handle_ = 0; Disable("CreateOverlay failed", err ? err : -1); return false; }
    vr::HmdMatrix34_t m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.f;
    m.m[0][3] = s_.x;
    m.m[1][3] = s_.y;
    m.m[2][3] = s_.z;
    if ((err = ((PFN_OvFloat)fn_[ver_->width])(handle_, s_.size)) != 0) { Disable("SetOverlayWidthInMeters failed", err); return false; }
    if ((err = ((PFN_OvTransform)fn_[ver_->transformDeviceRelative])(handle_, kHmd, &m)) != 0) {
        Disable("SetOverlayTransformTrackedDeviceRelative failed", err);
        return false;
    }
    ((PFN_OvUint)fn_[ver_->sortOrder])(handle_, 1000);   // above other overlays of this game (not fatal)
    LOGI("indicator: SteamVR overlay %s created through FnTable:%s -- %.3f m wide at (%.2f, %.2f, %.2f) m from the head; "
         "drawn by the compositor after the capture, never in the recording", key, ver_->name, s_.size, s_.x, s_.y, s_.z);
    return true;
}

bool OvrIndicator::FrameImpl(PFN_VR_GetGenericInterface get, ID3D11Device* dev, IndShow show, double now) {
    if (show == IndShow::Hidden) {
        if (visible_ && handle_) {
            ((PFN_OvHandle)fn_[ver_->hide])(handle_);
            visible_ = false;
        }
        return true;
    }
    if (!tried_ && !Create(get, dev)) return false;
    if (!handle_) return false;
    if (show != uploaded_) {
        // the picture at full strength (the recording pulse goes through the overlay alpha); bytes are sRGB-encoded
        std::vector<uint8_t> px((size_t)kIndicatorPx * kIndicatorPx * 4);
        RenderIndicator(show, 0.25, s_.opacity, true, false, px.data());   // now = 0.25: the pulse's peak (sin = 1)
        ctx_->UpdateSubresource(tex_.Get(), 0, nullptr, px.data(), kIndicatorPx * 4, 0);
        vr::Texture_t t{tex_.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        const int err = ((PFN_OvTexture)fn_[ver_->setTexture])(handle_, &t);
        if (err) { Disable("SetOverlayTexture failed", err); return false; }
        uploaded_ = show;
    }
    // recording: pulse at most 30 times a second (each call is a message to the compositor); other states: steady
    const float a = show == IndShow::Recording ? PulseAlpha(now) : 1.f;
    if (a != alpha_ && (show != IndShow::Recording || alpha_ < 0 || now - lastAlphaAt_ >= 1.0 / 30)) {
        ((PFN_OvFloat)fn_[ver_->alpha])(handle_, a);
        alpha_ = a;
        lastAlphaAt_ = now;
    }
    if (!visible_) {
        const int err = ((PFN_OvHandle)fn_[ver_->show])(handle_);
        if (err) { Disable("ShowOverlay failed", err); return false; }
        visible_ = true;
        if (!loggedShown_) {
            loggedShown_ = true;
            LOGI("indicator: dot shown in the headset (state %s)", IndShowName(show));
        }
    }
    return true;
}

namespace {
// SEH around the runtime's overlay calls: no C++ objects with destructors in this function.
bool FrameSafe(OvrIndicator* self, bool (OvrIndicator::*impl)(PFN_VR_GetGenericInterface, ID3D11Device*, IndShow, double),
               PFN_VR_GetGenericInterface get, ID3D11Device* dev, IndShow show, double now, bool* fault) {
    __try {
        return (self->*impl)(get, dev, show, now);
    } __except (GetExceptionCode() == 0xE06D7363ul ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return false;
    }
}
}

void OvrIndicator::Frame(PFN_VR_GetGenericInterface get, ID3D11Device* dev, IndShow show, double now) {
    if (disabled_ || !s_.enabled) return;
    bool fault = false;
    try {
        FrameSafe(this, &OvrIndicator::FrameImpl, get, dev, show, now, &fault);
    } catch (...) {
        Disable("C++ exception in the dot code", 0);
        return;
    }
    if (fault) Disable("fault inside the runtime's IVROverlay calls", 0);
}

void OvrIndicator::Destroy() {
    if (handle_ && ver_) {
        __try { ((PFN_OvHandle)fn_[ver_->destroy])(handle_); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        LOGI("indicator: SteamVR overlay destroyed");
    }
    handle_ = 0;
    visible_ = false;
    tex_.Reset();
    ctx_.Reset();
}
