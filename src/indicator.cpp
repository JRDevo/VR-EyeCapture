// indicator -- see indicator.h.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
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
    case IndShow::Live:      return "live";
    default:                 return "hidden";
    }
}

bool IndicatorTracker::NeedsMessage(RecState st) const {
    return st != prev_ && st == RecState::Idle && IsActive(prev_);
}

IndShow IndicatorTracker::Update(RecState st, const std::string& message, double now, bool live) {
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
    return live ? IndShow::Live : IndShow::Hidden;
}

void RenderIndicator(IndShow show, double now, float opacity, bool srgbTexture, bool bgra, uint8_t* out) {
    double r = 235, g = 35, b = 35;                                           // sRGB 0..255
    if (show == IndShow::Starting) { r = 255; g = 176; b = 0; }
    else if (show == IndShow::Saved) { r = 40; g = 205; b = 70; }
    else if (show == IndShow::Live) { r = 150; g = 60; b = 235; }   // violet, steady
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

VrIndicator::~VrIndicator() {
    WaitIdle12();
    if (fenceEvent12_) CloseHandle(fenceEvent12_);
}

bool VrIndicator::Create(XrSession session, const IndicatorDispatch& d) {
    if (api_ == GfxApi::D3D12) {
        if (!dev12_ || !queue12_) { LOGW("indicator: D3D12 session without device / queue -- indicator off"); return false; }
        const D3D12_COMMAND_QUEUE_DESC qd = queue12_->GetDesc();
        if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            LOGI("indicator: the game's D3D12 queue is not a direct queue (type %d) -- no in-headset recording indicator "
                 "(capture unaffected)", (int)qd.Type);
            return false;
        }
    } else if (api_ != GfxApi::D3D11 || !dev11_) {
        LOGI("indicator: not available on %s yet -- no in-headset recording indicator (capture unaffected)", GfxApiName(api_));
        return false;
    }
    if (!d.EnumerateSwapchainFormats || !d.CreateSwapchain || !d.DestroySwapchain || !d.EnumerateSwapchainImages ||
        !d.AcquireSwapchainImage || !d.WaitSwapchainImage || !d.ReleaseSwapchainImage || !d.CreateReferenceSpace || !d.DestroySpace) {
        LOGW("indicator: next layer / runtime did not provide the swapchain / space functions -- indicator off");
        return false;
    }
    if (dev11_) dev11_->GetImmediateContext(&ctx11_);
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
    images_.clear();
    images12_.clear();
    if (api_ == GfxApi::D3D12) {
        std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if (XR_SUCCEEDED(r) && n) r = d.EnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        if (XR_FAILED(r) || !n) { LOGW("indicator: xrEnumerateSwapchainImages failed (%d) -- indicator off", (int)r); return false; }
        for (auto& im : imgs) images12_.push_back(im.texture);
        if (!CreateD3D12(n)) return false;
    } else {
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_SUCCEEDED(r) && n) r = d.EnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        if (XR_FAILED(r) || !n) { LOGW("indicator: xrEnumerateSwapchainImages failed (%d) -- indicator off", (int)r); return false; }
        for (auto& im : imgs) images_.push_back(im.texture);
    }
    XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    si.poseInReferenceSpace.orientation.w = 1;
    r = d.CreateReferenceSpace(session, &si, &space_);
    if (XR_FAILED(r)) { space_ = XR_NULL_HANDLE; LOGW("indicator: xrCreateReferenceSpace(VIEW) failed (%d) -- indicator off", (int)r); return false; }
    pixels_.assign((size_t)kIndicatorPx * kIndicatorPx * 4, 0);
    LOGI("indicator: %ux%u %s swapchain (%u images) + VIEW space created -- quad layer at (%.2f, %.2f, %.2f) m, %.3f m, "
         "headset only%s",
         kIndicatorPx, kIndicatorPx, DxgiFormatName((DXGI_FORMAT)chosen), n, s_.x, s_.y, s_.z, s_.size,
         api_ == GfxApi::D3D12 ? " (D3D12: upload buffer + CopyTextureRegion on the game's queue)" : "");
    return true;
}

bool VrIndicator::CreateD3D12(uint32_t n) {
    if (images12_.empty() || !images12_[0]) { LOGW("indicator: D3D12 swapchain without images -- indicator off"); return false; }
    const D3D12_RESOURCE_DESC rd = images12_[0]->GetDesc();
    UINT64 total = 0;
    dev12_->GetCopyableFootprints(&rd, 0, 1, 0, &foot12_, nullptr, nullptr, &total);
    if (!total || foot12_.Footprint.RowPitch < (UINT)kIndicatorPx * 4) { LOGW("indicator: D3D12 copy footprint unusable -- indicator off"); return false; }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    up12_.assign(n, Upload12Slot());
    for (uint32_t i = 0; i < n; ++i) {
        Upload12Slot& u = up12_[i];
        HRESULT hr = dev12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&u.buffer));
        if (FAILED(hr)) { LOGW("indicator: D3D12 upload buffer failed hr=0x%08lx -- indicator off", (unsigned long)hr); return false; }
        void* p = nullptr;
        const D3D12_RANGE none{0, 0};   // the CPU never reads it
        if (FAILED(u.buffer->Map(0, &none, &p)) || !p) { LOGW("indicator: D3D12 upload buffer Map failed -- indicator off"); return false; }
        u.mapped = static_cast<uint8_t*>(p);
        hr = dev12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&u.alloc));
        if (FAILED(hr)) { LOGW("indicator: D3D12 command allocator failed hr=0x%08lx -- indicator off", (unsigned long)hr); return false; }
    }
    HRESULT hr = dev12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, up12_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&list12_));
    if (FAILED(hr)) { LOGW("indicator: D3D12 command list failed hr=0x%08lx -- indicator off", (unsigned long)hr); return false; }
    list12_->Close();
    hr = dev12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12_));
    if (FAILED(hr)) { LOGW("indicator: D3D12 fence failed hr=0x%08lx -- indicator off", (unsigned long)hr); return false; }
    if (!fenceEvent12_) fenceEvent12_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return fenceEvent12_ != nullptr;
}

bool VrIndicator::Upload12(uint32_t idx) {
    if (idx >= up12_.size() || idx >= images12_.size()) return false;
    Upload12Slot& u = up12_[idx];
    if (u.fenceValue && fence12_->GetCompletedValue() < u.fenceValue) {   // the previous copy from this buffer is still queued
        if (FAILED(fence12_->SetEventOnCompletion(u.fenceValue, fenceEvent12_)) || WaitForSingleObject(fenceEvent12_, 100) != WAIT_OBJECT_0)
            return false;
    }
    const UINT pitch = foot12_.Footprint.RowPitch;
    for (int y = 0; y < kIndicatorPx; ++y)
        memcpy(u.mapped + foot12_.Offset + (size_t)y * pitch, pixels_.data() + (size_t)y * kIndicatorPx * 4, (size_t)kIndicatorPx * 4);
    if (FAILED(u.alloc->Reset()) || FAILED(list12_->Reset(u.alloc.Get(), nullptr))) return false;
    // XR_KHR_D3D12_enable: a waited colour image is in RENDER_TARGET and must be back in it at release
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = images12_[idx];
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list12_->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = images12_[idx];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = u.buffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = foot12_;
    list12_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list12_->ResourceBarrier(1, &b);
    if (FAILED(list12_->Close())) return false;
    ID3D12CommandList* lists[] = {list12_.Get()};
    queue12_->ExecuteCommandLists(1, lists);
    u.fenceValue = ++fenceNext12_;
    return SUCCEEDED(queue12_->Signal(fence12_.Get(), u.fenceValue));
}

void VrIndicator::WaitIdle12() {
    if (!fence12_ || !fenceNext12_ || fence12_->GetCompletedValue() >= fenceNext12_) return;
    if (fenceEvent12_ && SUCCEEDED(fence12_->SetEventOnCompletion(fenceNext12_, fenceEvent12_))) WaitForSingleObject(fenceEvent12_, 2000);
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
    if (XR_FAILED(r) || idx >= (api_ == GfxApi::D3D12 ? images12_.size() : images_.size())) { Disable(d, "xrAcquireSwapchainImage failed"); return false; }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;   // 100 ms
    r = d.WaitSwapchainImage(swapchain_, &wi);
    if (r != XR_SUCCESS) { Disable(d, "xrWaitSwapchainImage failed / timed out"); return false; }
    RenderIndicator(show, now, s_.opacity, srgb_, bgra_, pixels_.data());
    if (api_ == GfxApi::D3D12) {
        if (!Upload12(idx)) {   // the image is still acquired: release it before switching off
            XrSwapchainImageReleaseInfo ri0{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            d.ReleaseSwapchainImage(swapchain_, &ri0);
            Disable(d, "D3D12 upload failed (the last copy did not complete within 100 ms, or a command list call failed)");
            return false;
        }
    } else {
        ctx11_->UpdateSubresource(images_[idx], 0, nullptr, pixels_.data(), kIndicatorPx * 4, 0);
    }
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
    WaitIdle12();   // D3D12: our last copy into the swapchain has completed before the runtime frees its images
    if (swapchain_ != XR_NULL_HANDLE && d.DestroySwapchain) d.DestroySwapchain(swapchain_);
    if (space_ != XR_NULL_HANDLE && d.DestroySpace) d.DestroySpace(space_);
    swapchain_ = XR_NULL_HANDLE;
    space_ = XR_NULL_HANDLE;
    images_.clear();
    ctx11_.Reset();
    images12_.clear();
    for (Upload12Slot& u : up12_) if (u.buffer && u.mapped) u.buffer->Unmap(0, nullptr);
    up12_.clear();
    list12_.Reset();
    // fence12_ / fenceNext12_ stay: ~VrIndicator waits on them (nothing new is submitted after Destroy)
}

void VrIndicator::Disable(const IndicatorDispatch& d, const char* why) {
    if (why && !disabled_) LOGW("indicator: %s -- indicator off for this session (capture unaffected)", why);
    disabled_ = true;
    Destroy(d);
}
