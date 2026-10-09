// indicator -- see indicator.h.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "indicator.h"
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "log.h"

namespace {

const double kPi = 3.14159265358979323846;

bool IsActive(RecState s) { return s == RecState::Starting || s == RecState::Recording || s == RecState::Stopping; }

uint8_t ToByte(double v) { return (uint8_t)std::lround(std::min(1.0, std::max(0.0, v)) * 255.0); }

double SrgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

// Distance from p to the segment a-b.
double SegDist(double px, double py, double ax, double ay, double bx, double by) {
    const double vx = bx - ax, vy = by - ay;
    double t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    t = std::min(1.0, std::max(0.0, t));
    const double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

const char* IndShowName(IndShow s) {
    switch (s) {
    case IndShow::Starting:  return "starting";
    case IndShow::Recording: return "recording";
    case IndShow::Saved:     return "saved";
    case IndShow::Failed:    return "failed";
    default:                 return "hidden";
    }
}

bool IndicatorTracker::NeedsMessage(RecState st) const {
    return st != prev_ && st == RecState::Idle && IsActive(prev_);
}

IndShow IndicatorTracker::Update(RecState st, const std::string& message, double now) {
    if (st != prev_) {
        if (st == RecState::Idle && IsActive(prev_)) {
            const bool saved = message.compare(0, 6, "saved ") == 0;
            timed_ = saved ? IndShow::Saved : IndShow::Failed;
            until_ = now + (saved ? 1.5 : 2.0);
        } else if (st == RecState::Failed) {
            timed_ = IndShow::Failed;
            until_ = now + 2.0;
        } else if (IsActive(st)) {
            timed_ = IndShow::Hidden;
        }
        prev_ = st;
    }
    if (st == RecState::Recording) return IndShow::Recording;
    if (st == RecState::Starting || st == RecState::Stopping) return IndShow::Starting;
    if (timed_ != IndShow::Hidden && now < until_) return timed_;
    timed_ = IndShow::Hidden;
    return IndShow::Hidden;
}

void RenderIndicator(IndShow show, double now, float opacity, bool srgbTexture, bool bgra, uint8_t* out) {
    double r = 235, g = 35, b = 35;                                           // sRGB 0..255
    if (show == IndShow::Starting) { r = 255; g = 176; b = 0; }
    else if (show == IndShow::Saved) { r = 40; g = 205; b = 70; }
    double scale = std::min(1.0, std::max(0.0, (double)opacity));
    if (show == IndShow::Recording) scale *= 0.675 + 0.325 * std::sin(2 * kPi * now);   // 0.35 .. 1.0 at 1 Hz
    if (show == IndShow::Hidden) scale = 0;
    double c[3] = {r / 255, g / 255, b / 255};
    if (!srgbTexture) for (double& v : c) v = SrgbToLinear(v);
    const uint8_t cr = ToByte(c[0]), cg = ToByte(c[1]), cb = ToByte(c[2]);
    const double mid = (kIndicatorPx - 1) * 0.5;   // pixel centres 0..63 -> 31.5
    for (int y = 0; y < kIndicatorPx; ++y) {
        for (int x = 0; x < kIndicatorPx; ++x) {
            double cov;
            if (show == IndShow::Failed) {   // red X: two 10 px strokes along the diagonals
                const double d = std::min(SegDist(x, y, mid - 22, mid - 22, mid + 22, mid + 22),
                                          SegDist(x, y, mid - 22, mid + 22, mid + 22, mid - 22));
                cov = 5.0 + 0.5 - d;
            } else {                          // anti-aliased disc, radius 26 px
                const double dx = x - mid, dy = y - mid;
                cov = 26.0 + 0.5 - std::sqrt(dx * dx + dy * dy);
            }
            cov = std::min(1.0, std::max(0.0, cov));
            uint8_t* p = out + ((size_t)y * kIndicatorPx + x) * 4;
            // straight alpha: the colour stays the same everywhere (no dark fringe when the runtime filters it)
            p[0] = bgra ? cb : cr; p[1] = cg; p[2] = bgra ? cr : cb;
            p[3] = ToByte(cov * scale);
        }
    }
}

bool VrIndicator::Create(XrSession session, const IndicatorDispatch& d) {
    if (api_ != GfxApi::D3D11 || !dev11_) {
        LOGI("indicator: not available on %s yet -- no in-headset recording indicator (capture unaffected)", GfxApiName(api_));
        return false;
    }
    if (!d.EnumerateSwapchainFormats || !d.CreateSwapchain || !d.DestroySwapchain || !d.EnumerateSwapchainImages ||
        !d.AcquireSwapchainImage || !d.WaitSwapchainImage || !d.ReleaseSwapchainImage || !d.CreateReferenceSpace || !d.DestroySpace) {
        LOGW("indicator: next layer / runtime did not provide the swapchain / space functions -- indicator off");
        return false;
    }
    dev11_->GetImmediateContext(&ctx11_);
    uint32_t n = 0;
    std::vector<int64_t> fmts;
    if (XR_FAILED(d.EnumerateSwapchainFormats(session, 0, &n, nullptr)) || n == 0) { LOGW("indicator: xrEnumerateSwapchainFormats failed -- indicator off"); return false; }
    fmts.resize(n);
    if (XR_FAILED(d.EnumerateSwapchainFormats(session, n, &n, fmts.data()))) { LOGW("indicator: xrEnumerateSwapchainFormats failed -- indicator off"); return false; }
    const int64_t srgbList[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    const int64_t unormList[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    int64_t chosen = 0;
    for (int pass = 0; pass < 2 && !chosen; ++pass) {
        const bool wantSrgb = (pass == 0) == preferSrgb_;
        for (int64_t f : wantSrgb ? srgbList : unormList)
            if (std::find(fmts.begin(), fmts.end(), f) != fmts.end()) { chosen = f; break; }
    }
    if (!chosen) { LOGW("indicator: the runtime offers no 8-bit RGBA / BGRA swapchain format -- indicator off"); return false; }
    srgb_ = chosen == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bgra_ = chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || chosen == DXGI_FORMAT_B8G8R8A8_UNORM;

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = chosen;
    ci.sampleCount = 1;
    ci.width = ci.height = kIndicatorPx;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = d.CreateSwapchain(session, &ci, &swapchain_);
    if (XR_FAILED(r)) { swapchain_ = XR_NULL_HANDLE; LOGW("indicator: xrCreateSwapchain failed (%d) -- indicator off", (int)r); return false; }
    n = 0;
    r = d.EnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r) && n) r = d.EnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    if (XR_FAILED(r) || !n) { LOGW("indicator: xrEnumerateSwapchainImages failed (%d) -- indicator off", (int)r); return false; }
    images_.clear();
    for (auto& im : imgs) images_.push_back(im.texture);
    XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    si.poseInReferenceSpace.orientation.w = 1;
    r = d.CreateReferenceSpace(session, &si, &space_);
    if (XR_FAILED(r)) { space_ = XR_NULL_HANDLE; LOGW("indicator: xrCreateReferenceSpace(VIEW) failed (%d) -- indicator off", (int)r); return false; }
    pixels_.assign((size_t)kIndicatorPx * kIndicatorPx * 4, 0);
    LOGI("indicator: %ux%u %s swapchain (%u images) + VIEW space created -- quad layer at (%.2f, %.2f, %.2f) m, %.3f m, "
         "headset only",
         kIndicatorPx, kIndicatorPx, DxgiFormatName((DXGI_FORMAT)chosen), n, s_.x, s_.y, s_.z, s_.size);
    return true;
}

bool VrIndicator::Frame(XrSession session, const IndicatorDispatch& d, IndShow show, double now, XrCompositionLayerQuad& quad) {
    if (disabled_ || show == IndShow::Hidden) return false;
    if (!Created()) {
        if (triedCreate_) return false;
        triedCreate_ = true;
        if (!Create(session, d)) { Disable(d, nullptr); return false; }
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r = d.AcquireSwapchainImage(swapchain_, &ai, &idx);
    if (XR_FAILED(r) || idx >= images_.size()) { Disable(d, "xrAcquireSwapchainImage failed"); return false; }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;   // 100 ms
    r = d.WaitSwapchainImage(swapchain_, &wi);
    if (r != XR_SUCCESS) { Disable(d, "xrWaitSwapchainImage failed / timed out"); return false; }
    RenderIndicator(show, now, s_.opacity, srgb_, bgra_, pixels_.data());
    ctx11_->UpdateSubresource(images_[idx], 0, nullptr, pixels_.data(), kIndicatorPx * 4, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    r = d.ReleaseSwapchainImage(swapchain_, &ri);
    if (XR_FAILED(r)) { Disable(d, "xrReleaseSwapchainImage failed"); return false; }

    quad = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    quad.space = space_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = swapchain_;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {kIndicatorPx, kIndicatorPx};
    quad.subImage.imageArrayIndex = 0;
    quad.pose.orientation = {0, 0, 0, 1};
    quad.pose.position = {s_.x, s_.y, s_.z};
    quad.size = {s_.size, s_.size};
    return true;
}

void VrIndicator::Destroy(const IndicatorDispatch& d) {
    if (swapchain_ != XR_NULL_HANDLE && d.DestroySwapchain) d.DestroySwapchain(swapchain_);
    if (space_ != XR_NULL_HANDLE && d.DestroySpace) d.DestroySpace(space_);
    swapchain_ = XR_NULL_HANDLE;
    space_ = XR_NULL_HANDLE;
    images_.clear();
    ctx11_.Reset();
}

void VrIndicator::Disable(const IndicatorDispatch& d, const char* why) {
    if (why && !disabled_) LOGW("indicator: %s -- indicator off for this session (capture unaffected)", why);
    disabled_ = true;
    Destroy(d);
}
