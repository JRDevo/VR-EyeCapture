// source_d3d12 -- IFrameSource for D3D12 games. See frame_source.h for the design.
//
// Ring textures are D3D12 committed resources on a D3D12_HEAP_FLAG_SHARED heap, opened on the encoder's D3D11 device
// with OpenSharedResource1; the two fences are D3D12 shared fences opened on the encoder device with OpenSharedFence.
// Game-thread work per frame: record one small command list (barriers + 2x CopyTextureRegion, + ResolveSubresource for
// MSAA swapchains) and submit it on the game's own queue between Wait(free) and Signal(ready).
//
// Resource states: per XR_KHR_D3D12_enable, a released colour swapchain image is in D3D12_RESOURCE_STATE_RENDER_TARGET
// (UNORDERED_ACCESS when the swapchain has only the UAV usage bit). We transition it to COPY_SOURCE and back. Ring
// textures live in COMMON between uses (the hand-off state across APIs).
#include "frame_source.h"
#include <d3d12.h>
#include <atomic>
#include <map>
#include <utility>
#include "log.h"

// XrSwapchainUsageFlags bits (avoid including openxr.h here)
static const uint64_t kUsageColor = 0x00000001;
static const uint64_t kUsageUav   = 0x00000004;

namespace {

class D3D12Source final : public IFrameSource {
public:
    bool Init(ID3D12Device* dev, ID3D12CommandQueue* q) {
        dev_ = dev;
        queue_ = q;
        const D3D12_COMMAND_QUEUE_DESC qd = queue_->GetDesc();
        listType_ = qd.Type;
        luid_ = dev_->GetAdapterLuid();
        LOGI("D3D12 source: game queue type %d (%s)", (int)qd.Type,
             qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : qd.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute" : "copy/other");
        if (listType_ != D3D12_COMMAND_LIST_TYPE_DIRECT && listType_ != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
            listType_ != D3D12_COMMAND_LIST_TYPE_COPY) {
            LOGE("D3D12 source: unsupported queue type -- capture disabled");
            return false;
        }
        return true;
    }

    GfxApi Api() const override { return GfxApi::D3D12; }
    LUID AdapterLuid() const override { return luid_; }

    bool StageFormat(const SwapchainInfo& sc, DXGI_FORMAT* stage, bool* srgb) const override {
        const DXGI_FORMAT f = (DXGI_FORMAT)sc.format;
        *stage = StageFormatForDxgi(f);
        *srgb = IsSrgbDxgi(f);
        return *stage != DXGI_FORMAT_UNKNOWN;
    }

    bool CreateRing(ID3D11Device* encDev, UINT w, UINT h, DXGI_FORMAT fmt, UINT count, SharedRing& out) override {
        ComPtr<ID3D11Device1> enc1;
        ComPtr<ID3D11Device5> enc5;
        if (FAILED(encDev->QueryInterface(IID_PPV_ARGS(&enc1))) || FAILED(encDev->QueryInterface(IID_PPV_ARGS(&enc5)))) return false;
        out = SharedRing();
        out.format = fmt; out.width = w; out.height = h;
        slots_.clear();
        slots_.resize(count);

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (UINT i = 0; i < count; ++i) {
            HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&slots_[i].tex));
            if (FAILED(hr)) { LOGE("D3D12 source: shared ring texture %ux%u %s failed hr=0x%08lx", w, h, DxgiFormatName(fmt), (unsigned long)hr); return false; }
            HANDLE hnd = nullptr;
            hr = dev_->CreateSharedHandle(slots_[i].tex.Get(), nullptr, GENERIC_ALL, nullptr, &hnd);
            if (FAILED(hr)) { LOGE("D3D12 source: CreateSharedHandle(ring) failed hr=0x%08lx", (unsigned long)hr); return false; }
            ComPtr<ID3D11Texture2D> encTex;
            hr = enc1->OpenSharedResource1(hnd, IID_PPV_ARGS(&encTex));
            CloseHandle(hnd);
            if (FAILED(hr)) { LOGE("D3D12 source: OpenSharedResource1(ring) on the encoder device failed hr=0x%08lx", (unsigned long)hr); return false; }
            out.tex.push_back(encTex);
            hr = dev_->CreateCommandAllocator(listType_, IID_PPV_ARGS(&slots_[i].alloc));
            if (FAILED(hr)) { LOGE("D3D12 source: CreateCommandAllocator failed hr=0x%08lx", (unsigned long)hr); return false; }
        }
        HRESULT hr = dev_->CreateCommandList(0, listType_, slots_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&list_));
        if (FAILED(hr)) { LOGE("D3D12 source: CreateCommandList failed hr=0x%08lx", (unsigned long)hr); return false; }
        list_->Close();
        if (!MakeFence(enc5.Get(), ready_, out.ready) || !MakeFence(enc5.Get(), free_, out.free)) return false;
        readyValue_.store(0);
        return true;
    }

    uint64_t CopyEyes(UINT slot, const EyeView& L, const EyeView& R, UINT dstXRight, uint64_t waitFree,
                      const OverlayCopy* ov, UINT nOv) override {
        if (slot >= slots_.size() || !list_) return 0;
        Slot& s = slots_[slot];
        if (s.lastReady && ready_->GetCompletedValue() < s.lastReady) return 0;   // previous copy into this slot still on the GPU
        if (FAILED(s.alloc->Reset()) || FAILED(list_->Reset(s.alloc.Get(), nullptr))) return 0;

        // Distinct source images (both eyes may share one image).
        ID3D12Resource* imgs[2] = {static_cast<ID3D12Resource*>(L.image), static_cast<ID3D12Resource*>(R.image)};
        const EyeView* eyes[2] = {&L, &R};
        const int nImg = (imgs[1] && imgs[1] != imgs[0]) ? 2 : 1;
        // record_overlays: the copies whose family atlas exists; their distinct images (an eye image is in its copy /
        // resolve source state already) and the atlases they go to
        const UINT kMaxOv = 32;
        bool famUsed[kOverlayFamilies] = {};
        const UINT kMaxUse = 128;   // >= kMaxOverlayCopies (overlay.h): every copy the recorder packed
        const OverlayCopy* use[kMaxUse] = {};
        UINT nUse = 0;
        ID3D12Resource* ovImg[kMaxOv] = {};
        D3D12_RESOURCE_STATES ovFrom[kMaxOv] = {}, ovTo[kMaxOv] = {};
        UINT nOvImg = 0;
        for (UINT i = 0; ov && i < nOv && nUse < kMaxUse; ++i) {
            const OverlayCopy& c = ov[i];
            if (!c.image || !c.sc || !c.w || !c.h || c.family < 0 || c.family >= kOverlayFamilies) continue;
            if (!atlasReady_[c.family].load(std::memory_order_acquire) || slot >= atlas_[c.family].size()) continue;
            const bool msaa = c.sc->sampleCount > 1;
            if (msaa && listType_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                if (!warnedOvResolveQueue_) { LOGW("D3D12 source: MSAA overlay swapchain on a non-direct queue -- cannot resolve, layer not recorded"); warnedOvResolveQueue_ = true; }
                continue;
            }
            ID3D12Resource* r = static_cast<ID3D12Resource*>(c.image);
            bool dup = r == imgs[0] || r == imgs[1];
            for (UINT k = 0; k < nOvImg && !dup; ++k) dup = ovImg[k] == r;
            if (!dup) {
                if (nOvImg >= kMaxOv) continue;
                ovImg[nOvImg] = r;
                ovFrom[nOvImg] = ScState(c.sc);
                ovTo[nOvImg] = msaa ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
                ++nOvImg;
            }
            famUsed[c.family] = true;
            use[nUse++] = &c;
        }

        D3D12_RESOURCE_BARRIER b[4 + kMaxOv + kOverlayFamilies]{};
        int nb = 0;
        for (int i = 0; i < nImg; ++i) if (imgs[i]) b[nb++] = Transition(imgs[i], SwapchainState(*eyes[i]), PreCopyState(*eyes[i]));
        b[nb++] = Transition(s.tex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT k = 0; k < nOvImg; ++k) b[nb++] = Transition(ovImg[k], ovFrom[k], ovTo[k]);
        for (int f = 0; f < kOverlayFamilies; ++f)
            if (famUsed[f]) b[nb++] = Transition(atlas_[f][slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->ResourceBarrier(nb, b);

        CopyOne(s.tex.Get(), 0, L, 0);
        CopyOne(s.tex.Get(), dstXRight, R, 1);
        const void* lastKey = nullptr;
        UINT lastSub = ~0u;
        for (UINT i = 0; i < nUse; ++i) {
            const OverlayCopy& c = *use[i];
            ID3D12Resource* src = static_cast<ID3D12Resource*>(c.image);
            UINT sub = c.arrayIndex * (c.sc->mipCount ? c.sc->mipCount : 1);
            ID3D12Resource* rt = nullptr;
            if (c.sc->sampleCount > 1) {   // resolve into a per-swapchain texture, copy the rectangle from there
                rt = ResolveTarget(c.scKey, 2, *c.sc, "overlay");
                if (!rt) continue;
                D3D12_RESOURCE_BARRIER rb = Transition(rt, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                if (lastKey != c.scKey || lastSub != sub) {   // the same slice again (one sub-image per eye): resolved already
                    list_->ResourceBarrier(1, &rb);
                    list_->ResolveSubresource(rt, 0, src, sub, (DXGI_FORMAT)c.sc->format);
                    rb = Transition(rt, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                } else {
                    rb = Transition(rt, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                }
                list_->ResourceBarrier(1, &rb);
                lastKey = c.scKey;
                lastSub = sub;
                src = rt;
                sub = 0;
            }
            D3D12_TEXTURE_COPY_LOCATION d{}, sl{};
            d.pResource = atlas_[c.family][slot].Get(); d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            sl.pResource = src; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = sub;
            const D3D12_BOX box{(UINT)c.x, (UINT)c.y, 0, (UINT)c.x + c.w, (UINT)c.y + c.h, 1};
            list_->CopyTextureRegion(&d, c.dstX, c.dstY, 0, &sl, &box);
            if (rt) {
                const D3D12_RESOURCE_BARRIER rb = Transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                list_->ResourceBarrier(1, &rb);
            }
        }

        nb = 0;
        for (int i = 0; i < nImg; ++i) if (imgs[i]) b[nb++] = Transition(imgs[i], PreCopyState(*eyes[i]), SwapchainState(*eyes[i]));
        b[nb++] = Transition(s.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        for (UINT k = 0; k < nOvImg; ++k) b[nb++] = Transition(ovImg[k], ovTo[k], ovFrom[k]);
        for (int f = 0; f < kOverlayFamilies; ++f)
            if (famUsed[f]) b[nb++] = Transition(atlas_[f][slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        list_->ResourceBarrier(nb, b);
        if (FAILED(list_->Close())) return 0;

        if (waitFree) queue_->Wait(free_.Get(), waitFree);
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t v = readyValue_.load() + 1;
        queue_->Signal(ready_.Get(), v);
        readyValue_.store(v);
        s.lastReady = v;
        return v;
    }

    bool CreateOverlayAtlas(ID3D11Device* encDev, UINT w, UINT h, DXGI_FORMAT format, UINT count, int family,
                            std::vector<ComPtr<ID3D11Texture2D>>& out) override {
        out.clear();
        if (family < 0 || family >= kOverlayFamilies || atlasReady_[family].load()) return false;   // once per family and ring
        ComPtr<ID3D11Device1> enc1;
        if (FAILED(encDev->QueryInterface(IID_PPV_ARGS(&enc1)))) return false;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = format;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // as the ring: what cross-API sharing of a texture needs
        std::vector<ComPtr<ID3D12Resource>> game;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<ID3D12Resource> r;
            HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&r));
            if (FAILED(hr)) { LOGE("D3D12 source: overlay atlas %ux%u %s failed hr=0x%08lx", w, h, DxgiFormatName(format), (unsigned long)hr); out.clear(); return false; }
            HANDLE hnd = nullptr;
            hr = dev_->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &hnd);
            if (FAILED(hr)) { LOGE("D3D12 source: CreateSharedHandle(overlay atlas) failed hr=0x%08lx", (unsigned long)hr); out.clear(); return false; }
            ComPtr<ID3D11Texture2D> encTex;
            hr = enc1->OpenSharedResource1(hnd, IID_PPV_ARGS(&encTex));
            CloseHandle(hnd);
            if (FAILED(hr)) { LOGE("D3D12 source: OpenSharedResource1(overlay atlas) on the encoder device failed hr=0x%08lx", (unsigned long)hr); out.clear(); return false; }
            out.push_back(encTex);
            game.push_back(r);
        }
        atlas_[family] = std::move(game);
        atlasReady_[family].store(true, std::memory_order_release);
        return true;
    }

    void DestroyRing() override {
        const uint64_t last = readyValue_.load();
        if (ready_ && last && ready_->GetCompletedValue() < last) {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (e) {
                if (SUCCEEDED(ready_->SetEventOnCompletion(last, e))) WaitForSingleObject(e, 2000);
                CloseHandle(e);
            }
        }
        for (int f = 0; f < kOverlayFamilies; ++f) {
            atlasReady_[f].store(false);
            atlas_[f].clear();
        }
        list_.Reset();
        slots_.clear();
        ready_.Reset();
        free_.Reset();
    }

    void ForgetSwapchain(const void* key) override {
        for (auto it = resolve_.begin(); it != resolve_.end();)
            it = (it->first.first == key) ? resolve_.erase(it) : std::next(it);
    }

private:
    struct Slot {
        ComPtr<ID3D12Resource> tex;
        ComPtr<ID3D12CommandAllocator> alloc;
        uint64_t lastReady = 0;
    };

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        return b;
    }
    static D3D12_RESOURCE_STATES SwapchainState(const EyeView& e) {
        if (e.sc && (e.sc->usageFlags & kUsageColor)) return D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (e.sc && (e.sc->usageFlags & kUsageUav)) return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    static D3D12_RESOURCE_STATES ScState(const SwapchainInfo* sc) {
        if (sc && (sc->usageFlags & kUsageColor)) return D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (sc && (sc->usageFlags & kUsageUav)) return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    static D3D12_RESOURCE_STATES PreCopyState(const EyeView& e) {
        return (e.sc && e.sc->sampleCount > 1) ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    bool MakeFence(ID3D11Device5* enc5, ComPtr<ID3D12Fence>& gameSide, ComPtr<ID3D11Fence>& encSide) {
        HRESULT hr = dev_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gameSide));
        if (FAILED(hr)) { LOGE("D3D12 source: CreateFence(SHARED) failed hr=0x%08lx", (unsigned long)hr); return false; }
        HANDLE h = nullptr;
        hr = dev_->CreateSharedHandle(gameSide.Get(), nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) { LOGE("D3D12 source: fence CreateSharedHandle failed hr=0x%08lx", (unsigned long)hr); return false; }
        hr = enc5->OpenSharedFence(h, IID_PPV_ARGS(&encSide));
        CloseHandle(h);
        if (FAILED(hr)) { LOGE("D3D12 source: OpenSharedFence on the encoder device failed hr=0x%08lx", (unsigned long)hr); return false; }
        return true;
    }

    void CopyOne(ID3D12Resource* dst, UINT dstX, const EyeView& e, int eye) {
        if (!e.image || !e.sc || e.w == 0 || e.h == 0) return;
        ID3D12Resource* src = static_cast<ID3D12Resource*>(e.image);
        const UINT mips = e.sc->mipCount ? e.sc->mipCount : 1;
        const UINT arr = e.sc->arraySize ? e.sc->arraySize : 1;
        UINT sub = e.arrayIndex * mips;   // mip 0 of the slice (plane 0)
        (void)arr;
        if (e.sc->sampleCount > 1) {
            if (listType_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                if (!warnedResolveQueue_) { LOGW("D3D12 source: MSAA swapchain on a non-direct queue -- cannot resolve, eye skipped"); warnedResolveQueue_ = true; }
                return;
            }
            ID3D12Resource* rt = ResolveTarget(e.swapchainKey, eye, *e.sc, eye ? "eye 1" : "eye 0");
            if (!rt) return;
            D3D12_RESOURCE_BARRIER b = Transition(rt, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            list_->ResourceBarrier(1, &b);
            list_->ResolveSubresource(rt, 0, src, sub, (DXGI_FORMAT)e.sc->format);
            b = Transition(rt, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list_->ResourceBarrier(1, &b);
            src = rt;
            sub = 0;
        }
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = sub;
        D3D12_BOX box{(UINT)e.x, (UINT)e.y, 0, (UINT)e.x + e.w, (UINT)e.y + e.h, 1};
        list_->CopyTextureRegion(&d, dstX, 0, 0, &s, &box);
        if (src != e.image) {
            D3D12_RESOURCE_BARRIER b = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            list_->ResourceBarrier(1, &b);
        }
    }

    // idx: 0 / 1 = eye, 2 = overlay layer
    ID3D12Resource* ResolveTarget(const void* scKey, int idx, const SwapchainInfo& sc, const char* what) {
        auto key = std::make_pair(scKey, idx);
        auto it = resolve_.find(key);
        if (it != resolve_.end()) return it->second.Get();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = sc.width; rd.Height = sc.height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = (DXGI_FORMAT)sc.format; rd.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> r;
        HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr)) {
            LOGE("D3D12 source: MSAA resolve target %ux%u %s failed hr=0x%08lx", sc.width, sc.height,
                 DxgiFormatName(rd.Format), (unsigned long)hr);
            resolve_[key] = nullptr;
            return nullptr;
        }
        LOGI("D3D12 source: MSAA swapchain (%ux) -> resolving %s through a %ux%u %s texture", sc.sampleCount, what,
             sc.width, sc.height, DxgiFormatName(rd.Format));
        resolve_[key] = r;
        return r.Get();
    }

    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    D3D12_COMMAND_LIST_TYPE listType_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
    LUID luid_{};
    std::vector<Slot> slots_;
    std::vector<ComPtr<ID3D12Resource>> atlas_[kOverlayFamilies];   // record_overlays: per family and slot (COMMON between uses, like the ring)
    std::atomic<bool> atlasReady_[kOverlayFamilies] = {};
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> ready_, free_;
    std::atomic<uint64_t> readyValue_{0};
    std::map<std::pair<const void*, int>, ComPtr<ID3D12Resource>> resolve_;
    bool warnedResolveQueue_ = false, warnedOvResolveQueue_ = false;
};

} // namespace

std::unique_ptr<IFrameSource> CreateD3D12FrameSource(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!device || !queue) return nullptr;
    auto s = std::make_unique<D3D12Source>();
    if (!s->Init(device, queue)) return nullptr;
    return s;
}
