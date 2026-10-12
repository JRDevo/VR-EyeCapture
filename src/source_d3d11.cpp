// source_d3d11 -- IFrameSource for D3D11 games. See frame_source.h for the design.
//
// Ring textures are created on the encoder device (shared NT handle) and opened on the game device; the two fences
// are created on the game device (shared) and opened on the encoder device. Game-thread work per frame:
// Wait(free) + 2x CopySubresourceRegion (+ ResolveSubresource for MSAA swapchains) + Signal(ready) + Flush, all on the
// game's immediate context. CopySubresourceRegion / Signal / Wait do not change any pipeline state the game set.
#include "frame_source.h"
#include <map>
#include <atomic>
#include <string>
#include <utility>
#include "log.h"

namespace {

class D3D11Source final : public IFrameSource {
public:
    bool Init(ID3D11Device* dev) {
        dev_ = dev;
        dev_->GetImmediateContext(&ctx_);
        if (FAILED(dev_.As(&dev5_)) || FAILED(ctx_.As(&ctx4_)) || FAILED(dev_.As(&dev1_))) {
            LOGE("D3D11 source: the game's device has no ID3D11Device5 / ID3D11DeviceContext4 (shared fences need "
                 "Windows 10 1703+ and a WDDM 2.2 driver) -- capture disabled");
            return false;
        }
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (FAILED(dev_.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&ad))) {
            LOGE("D3D11 source: cannot get the game device's adapter -- capture disabled");
            return false;
        }
        luid_ = ad.AdapterLuid;
        const UINT flags = dev_->GetCreationFlags();
        LOGI("D3D11 source: game device on '%s', feature level %x, creation flags 0x%x%s", elog::Narrow(ad.Description).c_str(),
             (unsigned)dev_->GetFeatureLevel(), flags,
             (flags & D3D11_CREATE_DEVICE_SINGLETHREADED) ? " (SINGLETHREADED: ring setup runs on a worker thread anyway; watch for issues)" : "");
        return true;
    }

    GfxApi Api() const override { return GfxApi::D3D11; }
    LUID AdapterLuid() const override { return luid_; }

    bool StageFormat(const SwapchainInfo& sc, DXGI_FORMAT* stage, bool* srgb) const override {
        const DXGI_FORMAT f = (DXGI_FORMAT)sc.format;
        *stage = StageFormatForDxgi(f);
        *srgb = IsSrgbDxgi(f);
        return *stage != DXGI_FORMAT_UNKNOWN;
    }

    bool CreateRing(ID3D11Device* encDev, UINT w, UINT h, DXGI_FORMAT fmt, UINT count, SharedRing& out) override {
        ComPtr<ID3D11Device5> enc5;
        if (FAILED(encDev->QueryInterface(IID_PPV_ARGS(&enc5)))) return false;
        out = SharedRing();
        out.format = fmt; out.width = w; out.height = h;
        gameTex_.clear();
        for (UINT i = 0; i < count; ++i) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            ComPtr<ID3D11Texture2D> encTex;
            HRESULT hr = encDev->CreateTexture2D(&d, nullptr, &encTex);
            if (FAILED(hr)) { LOGE("D3D11 source: shared ring texture %ux%u %s failed hr=0x%08lx", w, h, DxgiFormatName(fmt), (unsigned long)hr); return false; }
            ComPtr<IDXGIResource1> res1;
            HANDLE hnd = nullptr;
            if (FAILED(encTex.As(&res1)) ||
                FAILED(hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hnd))) {
                LOGE("D3D11 source: CreateSharedHandle(ring) failed hr=0x%08lx", (unsigned long)hr);
                return false;
            }
            ComPtr<ID3D11Texture2D> gameTex;
            hr = dev1_->OpenSharedResource1(hnd, IID_PPV_ARGS(&gameTex));
            CloseHandle(hnd);
            if (FAILED(hr)) { LOGE("D3D11 source: OpenSharedResource1(ring) on the game device failed hr=0x%08lx", (unsigned long)hr); return false; }
            out.tex.push_back(encTex);
            gameTex_.push_back(gameTex);
        }
        if (!MakeFence(enc5.Get(), ready_, out.ready) || !MakeFence(enc5.Get(), free_, out.free)) return false;
        readyValue_.store(0);
        return true;
    }

    uint64_t CopyEyes(UINT slot, const EyeView& L, const EyeView& R, UINT dstXRight, uint64_t waitFree,
                      const OverlayCopy* ov, UINT nOv) override {
        if (slot >= gameTex_.size()) return 0;
        ID3D11Texture2D* dst = gameTex_[slot].Get();
        if (waitFree) ctx4_->Wait(free_.Get(), waitFree);
        CopyOne(dst, 0, L, 0);
        CopyOne(dst, dstXRight, R, 1);
        // record_overlays: the overlay sub-images into this slot's atlas of their family (plain copies, MSAA images
        // resolved into a per-swapchain texture first; no pipeline state touched)
        if (nOv && ov) {
            const void* lastKey = nullptr;
            UINT lastSub = ~0u;
            for (UINT i = 0; i < nOv; ++i) {
                const OverlayCopy& c = ov[i];
                if (!c.image || !c.sc || !c.w || !c.h || c.family < 0 || c.family >= kOverlayFamilies) continue;
                if (!atlasReady_[c.family].load(std::memory_order_acquire) || slot >= gameAtlas_[c.family].size()) continue;
                ID3D11Texture2D* src = static_cast<ID3D11Texture2D*>(c.image);
                UINT sub = D3D11CalcSubresource(0, c.arrayIndex, c.sc->mipCount ? c.sc->mipCount : 1);
                if (c.sc->sampleCount > 1) {
                    ID3D11Texture2D* rt = ResolveTarget(c.scKey, 2, *c.sc, "overlay");
                    if (!rt) continue;
                    if (lastKey != c.scKey || lastSub != sub)   // the same slice again (one sub-image per eye): resolved already
                        ctx_->ResolveSubresource(rt, 0, src, sub, (DXGI_FORMAT)c.sc->format);
                    lastKey = c.scKey;
                    lastSub = sub;
                    src = rt;
                    sub = 0;
                }
                const D3D11_BOX box{(UINT)c.x, (UINT)c.y, 0, (UINT)c.x + c.w, (UINT)c.y + c.h, 1};
                ctx_->CopySubresourceRegion(gameAtlas_[c.family][slot].Get(), 0, c.dstX, c.dstY, 0, src, sub, &box);
            }
        }
        const uint64_t v = readyValue_.load() + 1;
        ctx4_->Signal(ready_.Get(), v);
        ctx_->Flush();    // submit now so the encoder device's GPU wait can be satisfied
        readyValue_.store(v);
        return v;
    }

    bool CreateOverlayAtlas(ID3D11Device* encDev, UINT w, UINT h, DXGI_FORMAT format, UINT count, int family,
                            std::vector<ComPtr<ID3D11Texture2D>>& out) override {
        out.clear();
        if (family < 0 || family >= kOverlayFamilies || atlasReady_[family].load()) return false;   // once per family and ring
        std::vector<ComPtr<ID3D11Texture2D>> game;
        for (UINT i = 0; i < count; ++i) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = format;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            ComPtr<ID3D11Texture2D> encTex;
            HRESULT hr = encDev->CreateTexture2D(&d, nullptr, &encTex);
            if (FAILED(hr)) { LOGE("D3D11 source: overlay atlas %ux%u %s failed hr=0x%08lx", w, h, DxgiFormatName(format), (unsigned long)hr); out.clear(); return false; }
            ComPtr<IDXGIResource1> res1;
            HANDLE hnd = nullptr;
            if (FAILED(encTex.As(&res1)) ||
                FAILED(hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hnd))) {
                LOGE("D3D11 source: CreateSharedHandle(overlay atlas) failed hr=0x%08lx", (unsigned long)hr);
                out.clear();
                return false;
            }
            ComPtr<ID3D11Texture2D> gameTex;
            hr = dev1_->OpenSharedResource1(hnd, IID_PPV_ARGS(&gameTex));
            CloseHandle(hnd);
            if (FAILED(hr)) { LOGE("D3D11 source: OpenSharedResource1(overlay atlas) on the game device failed hr=0x%08lx", (unsigned long)hr); out.clear(); return false; }
            out.push_back(encTex);
            game.push_back(gameTex);
        }
        gameAtlas_[family] = std::move(game);
        atlasReady_[family].store(true, std::memory_order_release);
        return true;
    }

    void DestroyRing() override {
        const uint64_t last = readyValue_.load();
        if (ready_ && last) {
            const ULONGLONG t0 = GetTickCount64();
            while (ready_->GetCompletedValue() < last && GetTickCount64() - t0 < 2000) Sleep(1);
        }
        for (int f = 0; f < kOverlayFamilies; ++f) {
            atlasReady_[f].store(false);
            gameAtlas_[f].clear();
        }
        gameTex_.clear();
        ready_.Reset();
        free_.Reset();
    }

    void ForgetSwapchain(const void* key) override {
        for (auto it = resolve_.begin(); it != resolve_.end();)
            it = (it->first.first == key) ? resolve_.erase(it) : std::next(it);
    }

private:
    bool MakeFence(ID3D11Device5* enc5, ComPtr<ID3D11Fence>& gameSide, ComPtr<ID3D11Fence>& encSide) {
        HRESULT hr = dev5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gameSide));
        if (FAILED(hr)) { LOGE("D3D11 source: CreateFence(SHARED) on the game device failed hr=0x%08lx", (unsigned long)hr); return false; }
        HANDLE h = nullptr;
        hr = gameSide->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) { LOGE("D3D11 source: fence CreateSharedHandle failed hr=0x%08lx", (unsigned long)hr); return false; }
        hr = enc5->OpenSharedFence(h, IID_PPV_ARGS(&encSide));
        CloseHandle(h);
        if (FAILED(hr)) { LOGE("D3D11 source: OpenSharedFence on the encoder device failed hr=0x%08lx", (unsigned long)hr); return false; }
        return true;
    }

    void CopyOne(ID3D11Texture2D* dst, UINT dstX, const EyeView& e, int eye) {
        if (!e.image || !e.sc || e.w == 0 || e.h == 0) return;
        ID3D11Texture2D* src = static_cast<ID3D11Texture2D*>(e.image);
        UINT sub = D3D11CalcSubresource(0, e.arrayIndex, e.sc->mipCount ? e.sc->mipCount : 1);
        if (e.sc->sampleCount > 1) {
            ID3D11Texture2D* rt = ResolveTarget(e.swapchainKey, eye, *e.sc, eye ? "eye 1" : "eye 0");
            if (!rt) return;
            ctx_->ResolveSubresource(rt, 0, src, sub, (DXGI_FORMAT)e.sc->format);
            src = rt;
            sub = 0;
        }
        D3D11_BOX box{(UINT)e.x, (UINT)e.y, 0, (UINT)e.x + e.w, (UINT)e.y + e.h, 1};
        ctx_->CopySubresourceRegion(dst, 0, dstX, 0, 0, src, sub, &box);
    }

    // idx: 0 / 1 = eye, 2 = overlay layer
    ID3D11Texture2D* ResolveTarget(const void* scKey, int idx, const SwapchainInfo& sc, const char* what) {
        auto key = std::make_pair(scKey, idx);
        auto it = resolve_.find(key);
        if (it != resolve_.end()) return it->second.Get();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = sc.width; d.Height = sc.height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = (DXGI_FORMAT)sc.format;           // typed format (the resolve needs one)
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = 0;
        ComPtr<ID3D11Texture2D> t;
        HRESULT hr = dev_->CreateTexture2D(&d, nullptr, &t);
        if (FAILED(hr)) {
            LOGE("D3D11 source: MSAA resolve target %ux%u %s failed hr=0x%08lx", d.Width, d.Height, DxgiFormatName(d.Format), (unsigned long)hr);
            resolve_[key] = nullptr;
            return nullptr;
        }
        LOGI("D3D11 source: MSAA swapchain (%ux) -> resolving %s through a %ux%u %s texture", sc.sampleCount, what,
             d.Width, d.Height, DxgiFormatName(d.Format));
        resolve_[key] = t;
        return t.Get();
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11Device1> dev1_;
    ComPtr<ID3D11Device5> dev5_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11DeviceContext4> ctx4_;
    LUID luid_{};
    std::vector<ComPtr<ID3D11Texture2D>> gameTex_;
    std::vector<ComPtr<ID3D11Texture2D>> gameAtlas_[kOverlayFamilies];   // record_overlays: per family and slot, opened on the game device
    std::atomic<bool> atlasReady_[kOverlayFamilies] = {};
    ComPtr<ID3D11Fence> ready_, free_;
    std::atomic<uint64_t> readyValue_{0};
    std::map<std::pair<const void*, int>, ComPtr<ID3D11Texture2D>> resolve_;
};

} // namespace

std::unique_ptr<IFrameSource> CreateD3D11FrameSource(ID3D11Device* gameDevice) {
    if (!gameDevice) return nullptr;
    auto s = std::make_unique<D3D11Source>();
    if (!s->Init(gameDevice)) return nullptr;
    return s;
}
