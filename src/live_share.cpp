// live_share -- producer side of the live view. See live_share.h (what is shared, cost model) and live_share_proto.h
// (protocol).
//
// Threads:
//   * game thread (Frame, g_lock held): probe once per second, start / stop the worker, per-frame throttle, slot choice
//     and CopyEyes. Raw eyes at full size: it also publishes each frame (the only writer of the per-frame block fields).
//     The pass (re-projection / overlays / half size): it packs the overlays, fills a one-item mailbox for the worker
//     instead and never writes the seqlock part.
//   * worker thread (one per sharing episode): frame source + share device + rings + exported handles, publishes the ring
//     in the block. Raw eyes at full size: then waits for the stop event. The pass: then runs it for each mailbox item
//     (GPU-wait ready on the share device, overlay atlases on demand, overlays into the raw slot, filter + pass into an
//     exported slot, signal, publish) until the stop event.
//     Teardown on the same thread (DestroyRing may wait up to 2 s for the last copy). The game thread stops calling
//     CopyEyes before it sets the stop event.
// One producer per process (the first session that finds a consumer); one producer per block (a block held by another
// live producer -- a second game -- is left alone; the producer pid is claimed with an interlocked compare-exchange
// before the worker starts, and a producer that finds another pid there stops).
#include "live_share.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "live_share_proto.h"
#include "log.h"
#include "reproject.h"
#include "shared_status.h"   // EyeCapProcessAlive

namespace {
const UINT kLiveSlots = 4;   // exported ring: the latest slot + one being read + two to write into
const UINT kRawSlots = 3;    // re-projection: internal raw ring (one in the pass, one in the mailbox, one being written)
static_assert(kLiveSlots <= EYECAP_LIVE_MAX_SLOTS, "slot count");
static_assert(sizeof(void*) == 8, "x64 only (handle values are exchanged as uint64)");
const DWORD kTeardownStepMs = 15;   // pause between texture frees: each is its own short kernel free instead of one ~900 MB batch that blocks the game's GPU submit for several ms
const int64_t kSlowCall100ns = 40000;   // a game-thread share call longer than 4 ms counts as a stall
std::atomic<void*> g_owner{nullptr};   // the LiveShare::Impl that shares in this process (one at a time)

uint32_t HalfOf(uint32_t v) { return v > 1 ? (v + 1) / 2 : 1; }

bool SameHead(const HeadSmoothingSettings& a, const HeadSmoothingSettings& b) {
    return a.smoothing == b.smoothing && a.followDeg == b.followDeg && a.rollLock == b.rollLock && a.fovScale == b.fovScale &&
           a.edgeGuard == b.edgeGuard && a.symmetricFov == b.symmetricFov;
}
}

struct LiveShare::Impl {
    enum class St { Idle, Starting, Live, Stopping };
    // raw ring slot states (re-projection; guarded by mx)
    enum RawSt : int { kFree, kWriting, kPending, kTaken, kDone };
    struct ItemEye { bool valid; float q[4]; float p[3]; float fov[4]; UINT x0, w, h; };
    struct Item {
        UINT slot; uint64_t ready; int64_t dispNs; ItemEye eye[2];
        uint32_t nOv, nBehind; bool projUnpremul; OverlayDraw ov[kMaxOverlays];   // overlays (ov[0 .. nBehind) behind)
    };

    LiveShareParams p;
    St st = St::Idle;                        // game thread
    ULONGLONG nextProbe = 0;
    bool loggedOther = false, loggedVersion = false, loggedFormat = false, loggedDenied = false;
    bool ovSeen = false;                     // this session has shown overlay layers (record_overlays = 1; sticky)
    HANDLE map = nullptr;                    // the consumer's block, mapped for one episode (game thread opens / closes)
    EyeCapLive* shm = nullptr;
    // ---- episode (game thread sets before the worker starts; constant while it runs) ----
    UINT W = 0, H = 0, leftW = 0, leftH = 0, rightW = 0, rightH = 0;   // the raw eyes (source of every copy)
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool srgb = true;
    uint32_t mode = EYECAP_LIVE_REPROJ_CONFIG;   // what the consumer asked for at the start
    bool half = false;                       // the consumer asked for EYECAP_LIVE_SIZE_HALF at the start
    HeadSmoothingSettings head;              // config.ini keys at the start
    bool ovCfg = false;                      // record_overlays at the start
    SymmetricFov sym;                        // symmetric_fov output (valid = on for this episode)
    bool reproj = false;                     // the re-projection maps (mode config + smoothing and / or symmetric fov)
    bool ovOn = false;                       // overlays composited (mode config + record_overlays)
    bool wantStage = false;                  // the pass is wanted (reproj, overlays or half size)
    UINT taps = 1;                           // bilinear taps per axis of the pass (misc.w; 2 = half size)
    // output = the exported ring (re-projection: the pass's output; raw: = the raw eyes)
    UINT outW = 0, outH = 0, outLeftW = 0, outLeftH = 0, outRightW = 0, outRightH = 0;
    uint32_t outFlags = 0;
    // ---- worker ----
    std::thread worker;
    HANDLE stopEvt = nullptr, workEvt = nullptr;
    std::atomic<bool> ready{false}, done{false}, failed{false};
    std::atomic<bool> stage{false};          // the re-projection runs (worker decides before `ready`; may fall back to raw)
    std::unique_ptr<IFrameSource> src;       // worker creates; game thread uses only while Live
    ComPtr<ID3D11Device> shareDev;
    ComPtr<ID3D11DeviceContext> shareCtx;
    ComPtr<ID3D11DeviceContext4> shareCtx4;
    SharedRing ring;                         // the frame source's ring: raw = exported, re-projection = internal raw ring
    std::vector<ComPtr<ID3D11Texture2D>> outTex;   // exported ring (raw: = ring.tex)
    ComPtr<ID3D11Fence> outReady, outFree;   // exported fences (raw: = ring.ready / ring.free)
    std::vector<HANDLE> exported;            // NT handles published in the block (closed at teardown)
    std::atomic<bool> quickTeardown{false};  // session ended (destructor): no game frame to protect, teardown is not paced
    std::string stopWhy;                     // game thread sets before the stop event; worker logs it
    std::string adapter;
    // re-projection resources (worker)
    ComPtr<ID3D11VertexShader> rpVs;
    ComPtr<ID3D11PixelShader> rpPs;
    ComPtr<ID3D11Buffer> rpCb;
    ComPtr<ID3D11SamplerState> rpSmp;
    std::vector<ComPtr<ID3D11ShaderResourceView>> rawSrv;
    std::vector<ComPtr<ID3D11RenderTargetView>> outRtv;
    HeadSmoother smoother;                   // the live share's own filter (never the recorder's)
    uint64_t outCounter = 0;                 // outReady values (worker)
    uint64_t lastRawFree = 0;                // last value signalled on ring.free (worker)
    // game thread <-> worker (re-projection)
    std::mutex mx;
    int rawState[kRawSlots] = {};
    uint64_t rawDone[kRawSlots] = {};        // kDone: ring.free value after which the slot may be rewritten
    Item mailbox{};
    bool haveItem = false;
    // per-frame publishing state (raw: game thread; re-projection: worker)
    uint64_t slotVal[kLiveSlots] = {};       // ready value each slot holds (0 = none / rewritten)
    UINT lastSlot = kLiveSlots - 1;
    // per-frame state (game thread)
    int64_t nextDue = INT64_MIN;
    // counters (game thread and worker; read by the worker for the stop line after the game thread stopped sharing)
    std::atomic<uint64_t> shared{0}, droppedBusy{0}, droppedGpu{0}, decimated{0}, superseded{0}, passBusy{0};
    std::atomic<uint64_t> gameCalls{0}, gameSlow{0};
    std::atomic<int64_t> gameMax100ns{0};
    // re-projection stats (worker)
    uint64_t smFrames = 0, smGuarded = 0, smUncovered = 0, smCatch = 0, smNoPose = 0, smResets = 0;
    double smCorrSum = 0, smCorrMax = 0;
    ULONGLONG liveSince = 0;
    std::atomic<ULONGLONG> lastPublish{0};   // GetTickCount64 of the last published frame (Active)
    // overlays (record_overlays; the recorder's code, the live share's own atlases on the raw ring)
    OverlayAtlasSet ovl;
    bool ovLoggedFull = false, ovLoggedFirst = false;   // game thread / worker
    std::atomic<uint64_t> ovLayersIn{0}, ovNotReady{0}, ovFull{0}, ovSetupFailed{0}, ovBehind{0};   // game thread counts
    uint64_t ovFrames = 0, ovDraws = 0;      // worker

    explicit Impl(const LiveShareParams& prm) : p(prm) {}

    void CloseMap() {
        if (shm) UnmapViewOfFile(shm);
        if (map) CloseHandle(map);
        shm = nullptr;
        map = nullptr;
    }

    // Game thread, at most once per second while idle. true = a consumer is there and this process may share.
    bool Probe(ULONGLONG now) {
        HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, p.shmName.c_str());
        if (!m) {   // the normal case: nobody is watching
            if (GetLastError() == ERROR_ACCESS_DENIED && !loggedDenied) {   // once per session, not every second
                loggedDenied = true;
                LOGW("live share: block %s exists, but this game may not open it (access denied) -- the viewer (OBS) runs "
                     "elevated or at a higher integrity level with a plugin that creates it with default security; update "
                     "the plugin or start OBS without administrator rights", elog::Narrow(p.shmName).c_str());
            }
            return false;
        }
        auto* b = (EyeCapLive*)MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(EyeCapLive));
        if (!b) { CloseHandle(m); return false; }
        bool ok = false;
        if (b->magic != EYECAP_LIVE_MAGIC || b->version != EYECAP_LIVE_VERSION) {
            if (!loggedVersion) {
                loggedVersion = true;
                LOGW("live share: block %s has magic 0x%08x version %u (want 0x%08x / %u) -- consumer of another version, ignored",
                     elog::Narrow(p.shmName).c_str(), b->magic, b->version, (unsigned)EYECAP_LIVE_MAGIC, (unsigned)EYECAP_LIVE_VERSION);
            }
        } else if (EyeCapLiveFresh(b->consumerHeartbeat, now)) {
            const DWORD other = b->producerPid;
            if (other && other != GetCurrentProcessId() && EyeCapProcessAlive(other) && EyeCapLiveFresh(b->producerHeartbeat, now)) {
                if (!loggedOther) {
                    loggedOther = true;
                    LOGI("live share: a consumer is watching, but game pid %lu is already sharing -- not sharing this one",
                         (unsigned long)other);
                }
            } else {
                ok = true;
            }
        }
        if (!ok) { UnmapViewOfFile(b); CloseHandle(m); return false; }
        map = m;
        shm = b;
        return true;
    }

    // Game thread, after Probe (block mapped), right before Start: takes the block's producer pid in ONE interlocked step
    // -- free (0), our own, or a producer that is dead / has a stale heartbeat -> this pid -- so two games probing at the
    // same moment cannot both publish into it. Then, as its only writer: clears the ring fields (slotCount 0 = not live for
    // the consumer while the worker sets up, so it never pairs this pid with an old producer's handle values) and starts
    // the heartbeat (another game's Probe / Claim sees a fresh producer from here on, also while Starting). Released by
    // Teardown (producerPid = 0; the worker always runs it, also after a failed setup). false = another game has it.
    bool Claim(ULONGLONG now) {
        const LONG me = (LONG)GetCurrentProcessId();
        volatile LONG* pid = (volatile LONG*)&shm->producerPid;   // 4-byte aligned (static_assert in live_share_proto.h)
        const LONG old = *pid;
        if (old && old != me && EyeCapProcessAlive((DWORD)old) && EyeCapLiveFresh(shm->producerHeartbeat, now)) return false;
        if (InterlockedCompareExchange(pid, me, old) != old) return false;   // another game won
        EyeCapLiveBeginWrite(shm);
        shm->slotCount = 0;
        shm->latestValue = 0;
        EyeCapLiveEndWrite(shm);
        shm->producerHeartbeat = GetTickCount64();
        return *pid == me;   // else taken over in between (that game saw the old, stale heartbeat)
    }

    // this process holds the block (a fresh load every call: another game may take it over)
    bool Owner() const { return *(volatile const uint32_t*)&shm->producerPid == GetCurrentProcessId(); }

    // ---------------------------------------------------------------------------------------------- worker thread

    // Re-projection resources on the share device: the exported output ring + fences, raw-slot SRVs, output RTVs,
    // shaders, constant buffer, sampler. false = not possible (logged); the caller falls back to raw eyes.
    bool SetupStage() {
        UINT sup = 0;
        const UINT need = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        if (FAILED(shareDev->CheckFormatSupport(fmt, &sup)) || (sup & need) != need) {
            LOGW("live share: %s cannot be rendered on the share device -- live view falls back to the raw eyes", DxgiFormatName(fmt));
            return false;
        }
        ComPtr<ID3DBlob> vsCode, psCode, errs;
        HRESULT hr = D3DCompile(kReprojVs, sizeof(kReprojVs) - 1, "live_reproject_vs", nullptr, nullptr, "main", "vs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, &errs);
        if (SUCCEEDED(hr)) {
            errs.Reset();
            hr = D3DCompile(kReprojPs, sizeof(kReprojPs) - 1, "live_reproject_ps", nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, &errs);
        }
        if (FAILED(hr)) {
            LOGE("live share: re-projection shader compile failed hr=0x%08lx %s -- live view falls back to the raw eyes",
                 (unsigned long)hr, errs ? (const char*)errs->GetBufferPointer() : "");
            return false;
        }
        hr = shareDev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &rpVs);
        if (SUCCEEDED(hr)) hr = shareDev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &rpPs);
        if (SUCCEEDED(hr)) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(RpConstants);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = shareDev->CreateBuffer(&bd, nullptr, &rpCb);
        }
        if (SUCCEEDED(hr)) {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            hr = shareDev->CreateSamplerState(&sd, &rpSmp);
        }
        for (size_t i = 0; SUCCEEDED(hr) && i < ring.tex.size(); ++i) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = fmt;   // typed UNORM / float member of the copy family: no sRGB decode on sampling
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            ComPtr<ID3D11ShaderResourceView> v;
            hr = shareDev->CreateShaderResourceView(ring.tex[i].Get(), &sd, &v);
            rawSrv.push_back(v);
        }
        for (UINT i = 0; SUCCEEDED(hr) && i < kLiveSlots; ++i) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = outW; d.Height = outH; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            ComPtr<ID3D11Texture2D> t;
            hr = shareDev->CreateTexture2D(&d, nullptr, &t);
            ComPtr<ID3D11RenderTargetView> rtv;
            if (SUCCEEDED(hr)) hr = shareDev->CreateRenderTargetView(t.Get(), nullptr, &rtv);
            outTex.push_back(t);
            outRtv.push_back(rtv);
        }
        ComPtr<ID3D11Device5> dev5;
        if (SUCCEEDED(hr)) hr = shareDev.As(&dev5);
        if (SUCCEEDED(hr)) hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&outReady));
        if (SUCCEEDED(hr)) hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&outFree));
        if (FAILED(hr)) {
            LOGE("live share: re-projection setup on the share device failed hr=0x%08lx (output %ux%u %s) -- live view falls back "
                 "to the raw eyes", (unsigned long)hr, outW, outH, DxgiFormatName(fmt));
            return false;
        }
        smoother.Configure(head);   // also resets the filter: the episode starts at the actual head pose
        smoother.SetSymmetricOutput(sym.valid ? sym.tanH : 0.0, sym.valid ? sym.tanV : 0.0);
        return true;
    }

    void ReleaseStage() {
        ovl.Release();
        rawSrv.clear();
        outRtv.clear();
        outTex.clear();
        outReady.Reset();
        outFree.Reset();
        rpVs.Reset(); rpPs.Reset(); rpCb.Reset(); rpSmp.Reset();
    }

    bool Setup() {
        if (p.api == GfxApi::D3D11) src = CreateD3D11FrameSource(p.dev11);
        else if (p.api == GfxApi::D3D12) src = CreateD3D12FrameSource(p.dev12, p.queue12);
        if (!src) { LOGE("live share: no frame source for %s", GfxApiName(p.api)); return false; }
        shareDev = CreateEncoderDevice(src->AdapterLuid(), &adapter);
        if (!shareDev) { LOGE("live share: no share device"); return false; }
        shareDev->GetImmediateContext(&shareCtx);
        if (FAILED(shareCtx.As(&shareCtx4))) { LOGE("live share: no ID3D11DeviceContext4 on the share device"); return false; }
        const UINT n = wantStage ? kRawSlots : kLiveSlots;
        if (!src->CreateRing(shareDev.Get(), W, H, fmt, n, ring) || ring.tex.size() != n || !ring.ready || !ring.free) {
            LOGE("live share: ring creation failed (%ux%u %s)", W, H, DxgiFormatName(fmt));
            return false;
        }
        bool useStage = wantStage && SetupStage();
        if (wantStage && !useStage) {
            // fallback: raw eyes through the exported ring, which must then be the frame source's own kLiveSlots ring
            ReleaseStage();
            src->DestroyRing();
            ring = SharedRing();
            if (!src->CreateRing(shareDev.Get(), W, H, fmt, kLiveSlots, ring) || ring.tex.size() != kLiveSlots || !ring.ready || !ring.free) {
                LOGE("live share: ring creation failed (%ux%u %s)", W, H, DxgiFormatName(fmt));
                return false;
            }
        }
        if (!useStage) {
            outTex = ring.tex;
            outReady = ring.ready;
            outFree = ring.free;
            outW = W; outH = H; outLeftW = leftW; outLeftH = leftH; outRightW = rightW; outRightH = rightH;
            outFlags = 0;
        }
        stage.store(useStage);
        // export: NT handles of the share-device side (re-sharing an opened shared texture / fence works for both the
        // D3D11 ring, made on the share device, and the D3D12 one, made on the game device and opened here)
        uint64_t tex[EYECAP_LIVE_MAX_SLOTS] = {};
        for (UINT i = 0; i < kLiveSlots; ++i) {
            ComPtr<IDXGIResource1> r1;
            HANDLE h = nullptr;
            HRESULT hr = outTex[i].As(&r1);
            if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
            if (FAILED(hr)) { LOGE("live share: CreateSharedHandle(ring %u) failed hr=0x%08lx", i, (unsigned long)hr); return false; }
            exported.push_back(h);
            tex[i] = (uint64_t)(uintptr_t)h;
        }
        HANDLE hr_ = nullptr, hf = nullptr;
        HRESULT hr = outReady->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hr_);
        if (SUCCEEDED(hr)) { exported.push_back(hr_); hr = outFree->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hf); }
        if (FAILED(hr)) { LOGE("live share: fence CreateSharedHandle failed hr=0x%08lx", (unsigned long)hr); return false; }
        exported.push_back(hf);

        // publish the ring (the game thread does not touch the block's seqlock part until `ready`)
        if (!Owner()) { LOGW("live share: another game took the block over during the setup -- not sharing"); return false; }
        for (UINT i = 0; i < EYECAP_LIVE_MAX_SLOTS; ++i) InterlockedExchange64(&shm->slotValue[i], 0);
        InterlockedExchange64(&shm->claim, 0);   // a claim of the previous generation means nothing now
        EyeCapLiveBeginWrite(shm);
        shm->producerPid = GetCurrentProcessId();
        shm->generation = shm->generation + 1;
        shm->slotCount = kLiveSlots;
        for (UINT i = 0; i < EYECAP_LIVE_MAX_SLOTS; ++i) shm->texHandle[i] = tex[i];
        shm->readyHandle = (uint64_t)(uintptr_t)hr_;
        shm->freeHandle = (uint64_t)(uintptr_t)hf;
        shm->width = outW; shm->height = outH;
        shm->format = (uint32_t)fmt;
        shm->srgb = srgb ? 1u : 0u;
        shm->leftWidth = outLeftW; shm->leftHeight = outLeftH; shm->rightWidth = outRightW; shm->rightHeight = outRightH;
        shm->reprojected = outFlags;
        shm->latestSlot = 0;
        shm->latestValue = 0;
        shm->framesShared = 0;
        shm->framesDropped = 0;
        EyeCapCopy(shm->game, 64, p.game.c_str());
        EyeCapLiveEndWrite(shm);
        shm->producerHeartbeat = GetTickCount64();
        if (useStage) {
            const double outMb = (double)outW * outH * (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4) / (1024.0 * 1024.0);
            const double rawMb = (double)W * H * (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4) / (1024.0 * 1024.0);
            char what[160];
            if (reproj)
                snprintf(what, sizeof(what), "re-projected (%s%s)%s%s", sym.valid ? "symmetric fov" : "own fov",
                         head.Enabled() ? ", head smoothing" : "", ovOn ? " + overlays" : "", half ? ", half size" : "");
            else
                snprintf(what, sizeof(what), "raw eyes%s%s (identity pass)", ovOn ? " + overlays" : "", half ? ", half size" : "");
            LOGI("live share: ring ready -- generation %u, %u slots %ux%u %s %s from a %u-slot raw ring %ux%u on the share device "
                 "(+%.0f MB VRAM%s), %u x %u taps, handles exported to the consumer (adapter '%s')", shm->generation, kLiveSlots,
                 outW, outH, DxgiFormatName(fmt), what, kRawSlots, W, H, outMb * kLiveSlots + rawMb * kRawSlots,
                 ovOn ? " + the overlay atlases when the game shows overlays" : "", taps, taps, adapter.c_str());
        } else {
            LOGI("live share: ring ready -- generation %u, %u slots %ux%u %s raw eyes, handles exported to the consumer (adapter '%s')",
                 shm->generation, kLiveSlots, outW, outH, DxgiFormatName(fmt), adapter.c_str());
        }
        return true;
    }

    // Worker: an exported slot that is not the latest one and not being read by the consumer (CPU check only).
    bool SlotBusy(UINT s, uint64_t v) const {
        const int64_t c = shm->claim;
        return c && EYECAP_LIVE_CLAIM_SLOT(c) == s && EYECAP_LIVE_CLAIM_VALUE(c) == v && outFree->GetCompletedValue() < v;
    }

    UINT PickOutSlot() {
        const UINT latest = lastSlot;
        for (UINT k = 1; k <= kLiveSlots; ++k) {
            const UINT s = (lastSlot + k) % kLiveSlots;
            if (s == latest && slotVal[s]) continue;
            const uint64_t old = slotVal[s];
            if (old) {
                if (SlotBusy(s, old)) continue;
                InterlockedExchange64(&shm->slotValue[s], 0);   // full barrier: the consumer re-checks this after its claim
                if (SlotBusy(s, old)) continue;                 // claimed just now: left as it is (marked rewritten)
            }
            return s;
        }
        return kLiveSlots;
    }

    void Publish(UINT slot, uint64_t v) {
        if (!Owner()) return;   // another game took the block over: never write its seqlock part (the game thread stops)
        lastPublish.store(GetTickCount64(), std::memory_order_relaxed);
        slotVal[slot] = v;
        InterlockedExchange64(&shm->slotValue[slot], (int64_t)v);
        lastSlot = slot;
        const uint64_t n = ++shared;
        EyeCapLiveBeginWrite(shm);
        shm->latestSlot = slot;
        shm->latestValue = v;
        shm->framesShared = n;
        shm->framesDropped = droppedBusy.load() + droppedGpu.load() + superseded.load() + passBusy.load();
        EyeCapLiveEndWrite(shm);
    }

    // Worker, re-projection: one mailbox item -> filter -> pass into an exported slot -> publish.
    void Process(const Item& it) {
        const UINT slot = PickOutSlot();
        if (slot == kLiveSlots) {   // the consumer holds the others: drop this frame (nothing was read on the GPU)
            ++passBusy;
            std::lock_guard<std::mutex> lk(mx);
            rawState[it.slot] = kFree;
            return;
        }
        RpConstants rc{};
        float headCorr[4] = {0, 0, 0, 1};   // O * H^-1 of this frame: head-locked overlays follow the smoothed head
        bool haveHeadCorr = false;
        SmoothFrame sf;
        if (reproj) {
            SmoothEyeIn in[2];
            for (int e = 0; e < 2; ++e) in[e] = ReprojSmoothIn(it.eye[e].valid, it.eye[e].q, it.eye[e].fov);
            smoother.Update(it.dispNs, in, sf);
            ++smFrames;
            if (sf.guarded) ++smGuarded;
            if (sf.uncovered) ++smUncovered;
            if (sf.catchUp) ++smCatch;
            if (sf.noPose) ++smNoPose;
            if (sf.reset) ++smResets;
            smCorrSum += sf.correctionDeg;
            if (sf.correctionDeg > smCorrMax) smCorrMax = sf.correctionDeg;
            if (head.Enabled() && !sf.noPose) {   // the recorder's rule: smoothing off = overlays without any correction
                headCorr[0] = (float)sf.headCorr.x; headCorr[1] = (float)sf.headCorr.y;
                headCorr[2] = (float)sf.headCorr.z; headCorr[3] = (float)sf.headCorr.w;
                haveHeadCorr = true;
            }
        }
        for (int e = 0; e < 2; ++e) {
            RpEye& re = rc.eye[e];
            if (reproj) ReprojEyeMapping(sf.eye[e], re);
            else ReprojIdentityMapping(re);   // overlays only and / or half size: the source rectangle scaled into dst
            const ItemEye& ie = it.eye[e];
            re.src[0] = (float)ie.x0; re.src[1] = 0.0f;
            re.src[2] = (float)std::max<UINT>(1, ie.w); re.src[3] = (float)std::max<UINT>(1, ie.h);
            // the output eye's rectangle (the symmetric frustum or the raw layout, halved for Half)
            re.dst[0] = (float)(e == 0 ? 0 : outLeftW); re.dst[1] = 0.0f;
            re.dst[2] = (float)(e == 0 ? outLeftW : outRightW); re.dst[3] = (float)(e == 0 ? outLeftH : outRightH);
        }
        rc.misc[0] = 1.0f / (float)W; rc.misc[1] = 1.0f / (float)H; rc.misc[2] = (float)outLeftW; rc.misc[3] = (float)taps;
        rc.misc2[0] = 0.0f;

        shareCtx4->Wait(ring.ready.Get(), it.ready);   // the game's copy into the raw slot (and its overlay sub-images)
        // overlays: a new format family -> its atlas (the game thread packs that family from its next frame on)
        if (ovOn && ovl.Pending())
            ovl.MakePending(src.get(), shareDev.Get(), ring.tex, fmt, srgb, "live share", "shown in the live view", "share device",
                            "the live view's pass");
        if (it.nOv && ovl.comp.Ready()) {   // onto the raw slot (the live share's own copy), before the pass
            OverlayEye oe[2];
            for (int e = 0; e < 2; ++e) {
                const ItemEye& ie = it.eye[e];
                oe[e].valid = ie.valid;
                memcpy(oe[e].q, ie.q, sizeof(oe[e].q));
                memcpy(oe[e].p, ie.p, sizeof(oe[e].p));
                memcpy(oe[e].fov, ie.fov, sizeof(oe[e].fov));
                oe[e].x0 = ie.x0; oe[e].w = ie.w; oe[e].h = ie.h;
            }
            const int nd = ovl.comp.Draw(shareCtx.Get(), it.slot, oe, it.ov, it.nOv, it.nBehind, it.projUnpremul,
                                         haveHeadCorr ? headCorr : nullptr);
            if (nd) {
                ++ovFrames;
                ovDraws += (uint64_t)nd;
                if (!ovLoggedFirst) {
                    ovLoggedFirst = true;
                    LOGI("live share: first frame with overlays composited (%u layer(s), %d eye draws)", it.nOv, nd);
                }
            }
        }
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(shareCtx->Map(rpCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            memcpy(ms.pData, &rc, sizeof(rc));
            shareCtx->Unmap(rpCb.Get(), 0);
        }
        ID3D11ShaderResourceView* srv = rawSrv[it.slot].Get();
        ID3D11RenderTargetView* rtv = outRtv[slot].Get();
        ReprojDraw(shareCtx.Get(), rpVs.Get(), rpPs.Get(), rpCb.Get(), rpSmp.Get(), &srv, 1, &rtv, 1, outW, outH);
        const uint64_t v = ++outCounter;
        shareCtx4->Signal(outReady.Get(), v);          // the consumer's copy may start
        shareCtx4->Signal(ring.free.Get(), it.ready);  // the game may rewrite the raw slot once this completes
        lastRawFree = it.ready;
        shareCtx->Flush();
        {
            std::lock_guard<std::mutex> lk(mx);
            rawState[it.slot] = kDone;
            rawDone[it.slot] = it.ready;
        }
        Publish(slot, v);
    }

    void StageLoop() {
        HANDLE h[2] = {stopEvt, workEvt};
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, h, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0 + 1) return;   // stop (or a wait failure)
            Item it{};
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(mx);
                if (haveItem) {
                    it = mailbox;
                    haveItem = false;
                    rawState[it.slot] = kTaken;
                    have = true;
                }
            }
            if (have) Process(it);
        }
    }

    void Teardown() {
        // unpublish first: the consumer drops its imports (it keeps its own references alive meanwhile). Only while this
        // process holds the block (also the release of Claim after a failed setup); a block another game took over is
        // that game's -- no seqlock write from here.
        if (shm && Owner()) {
            EyeCapLiveBeginWrite(shm);
            shm->producerPid = 0;
            shm->slotCount = 0;
            shm->latestValue = 0;
            EyeCapLiveEndWrite(shm);
        }
        if (stage.load() && ring.free && lastRawFree) {   // our last pass has read its raw slot before the ring goes
            shareCtx->Flush();
            const ULONGLONG t0 = GetTickCount64();
            while (ring.free->GetCompletedValue() < lastRawFree && GetTickCount64() - t0 < 2000) Sleep(1);
        }
        if (src && ring.ready) src->DestroyRing();   // waits for the last copy on the game's GPU
        std::vector<ComPtr<ID3D11Texture2D>> rawTex = std::move(ring.tex);   // dropped one per step below
        ring = SharedRing();
        // the small things first (the consumer's imports are its own references)
        ovl.comp.Release();   // the compositor holds SRVs of the atlases
        rawSrv.clear();
        outRtv.clear();
        rpVs.Reset(); rpPs.Reset(); rpCb.Reset(); rpSmp.Reset();
        outReady.Reset();
        outFree.Reset();
        for (HANDLE h : exported) if (h) CloseHandle(h);
        exported.clear();
        shareCtx4.Reset();
        // then the big textures, one reference per step: each free is its own short kernel free instead of one ~900 MB batch
        auto step = [&]() {
            if (shareCtx) { shareCtx->ClearState(); shareCtx->Flush(); }
            if (!quickTeardown.load()) Sleep(kTeardownStepMs);
        };
        if (!stage.load()) outTex.clear();   // raw eyes: outTex = ring.tex, these references free nothing (rawTex holds them)
        while (ovl.ReleaseOneAtlas()) step();
        while (!outTex.empty()) { outTex.pop_back(); step(); }
        while (!rawTex.empty()) { rawTex.pop_back(); step(); }
        if (shareCtx) { shareCtx->ClearState(); shareCtx->Flush(); }
        if (!quickTeardown.load()) Sleep(kTeardownStepMs);
        ReleaseStage();   // whatever is left
        shareCtx.Reset();
        shareDev.Reset();
        src.reset();
    }

    void Run() {
        bool ok = false;
        try { ok = Setup(); } catch (...) { ok = false; LOGE("live share: setup threw"); }
        if (ok) {
            ready.store(true, std::memory_order_release);
            if (stage.load()) {
                try { StageLoop(); } catch (...) { LOGE("live share: re-projection worker threw -- waiting for the stop"); WaitForSingleObject(stopEvt, INFINITE); }
            } else {
                WaitForSingleObject(stopEvt, INFINITE);
            }
        } else {
            failed.store(true);
        }
        try { Teardown(); } catch (...) { LOGE("live share: teardown threw (ignored)"); }
        if (ok) {
            const double sec = (GetTickCount64() - liveSince) / 1000.0;
            const uint64_t calls = gameCalls.load();
            LOGI("live share: stopped (%s) after %.1f s -- frames shared %llu, dropped busy %llu (consumer reading / ring full), "
                 "dropped gpu %llu, skipped by max fps %llu, replaced before the pass %llu, pass found no free slot %llu | game thread: "
                 "%llu share calls, max %.2f ms, %llu over %.0f ms", stopWhy.c_str(), sec, (unsigned long long)shared.load(),
                 (unsigned long long)droppedBusy.load(), (unsigned long long)droppedGpu.load(), (unsigned long long)decimated.load(),
                 (unsigned long long)superseded.load(), (unsigned long long)passBusy.load(), (unsigned long long)calls,
                 gameMax100ns.load() / 1e4, (unsigned long long)gameSlow.load(), kSlowCall100ns / 1e4);
            if (ovLayersIn.load())
                LOGI("live share: overlay summary: %llu frames with overlays composited (%llu layer x eye draws) | layers handed "
                     "over %llu (%llu behind the projection layer): before their atlas existed %llu, atlas full %llu, atlas setup "
                     "failed %llu | atlases: 0x%x%s", (unsigned long long)ovFrames, (unsigned long long)ovDraws,
                     (unsigned long long)ovLayersIn.load(), (unsigned long long)ovBehind.load(),
                     (unsigned long long)ovNotReady.load(), (unsigned long long)ovFull.load(),
                     (unsigned long long)ovSetupFailed.load(), (unsigned)ovl.ready.load(),
                     ovl.failed.load() ? " | an atlas / compositor setup FAILED" : "");
            if (stage.load() && reproj)
                LOGI("live share: re-projection summary (head smoothing %s, symmetric fov %s): %llu frames | correction mean %.2f deg, "
                     "max %.2f deg | edge guard limited the output on %llu | uncovered (black) %llu | follow / catch-up %llu | no pose %llu "
                     "| filter resets %llu", head.Enabled() ? "on" : "off", sym.valid ? "on" : "off", (unsigned long long)smFrames,
                     smFrames ? smCorrSum / (double)smFrames : 0.0, smCorrMax, (unsigned long long)smGuarded,
                     (unsigned long long)smUncovered, (unsigned long long)smCatch, (unsigned long long)smNoPose,
                     (unsigned long long)smResets);
        }
        done.store(true, std::memory_order_release);
    }

    // ---------------------------------------------------------------------------------------------- game thread

    void Start(const EyeView& L, const EyeView& R, DXGI_FORMAT stageFmt, bool srgbEnc, const HeadSmoothingSettings& h,
               bool overlaysOn) {
        leftW = L.w; leftH = L.h; rightW = R.w; rightH = R.h;
        W = L.w + R.w;
        H = L.h > R.h ? L.h : R.h;
        fmt = stageFmt;
        srgb = srgbEnc;
        mode = shm->reproject == EYECAP_LIVE_REPROJ_RAW ? (uint32_t)EYECAP_LIVE_REPROJ_RAW : (uint32_t)EYECAP_LIVE_REPROJ_CONFIG;
        half = shm->size == EYECAP_LIVE_SIZE_HALF;
        head = h;
        ovCfg = overlaysOn;
        // the recorder's rules (Recorder::Start, flat): symmetric fov from the views' fov / size at this frame, fixed for
        // the episode; the re-projection maps with smoothing and / or the symmetric fov
        sym = SymmetricFov();
        if (mode == EYECAP_LIVE_REPROJ_CONFIG && head.symmetricFov && L.hasPose && R.hasPose)
            sym = ComputeSymmetricFov(L.fov, R.fov, L.w, L.h, R.w, R.h);
        reproj = mode == EYECAP_LIVE_REPROJ_CONFIG && (head.Enabled() || sym.valid);
        // overlays: only once this session has shown any (until then the plain copy path stays, at no extra cost; the
        // first overlay layer restarts the ring with them, see Frame)
        ovOn = mode == EYECAP_LIVE_REPROJ_CONFIG && ovCfg && ovSeen;
        wantStage = reproj || ovOn || half;
        taps = half ? 2 : 1;
        if (sym.valid) {
            outLeftW = outRightW = sym.eyeW;
            outLeftH = outRightH = sym.eyeH;
        } else {
            outLeftW = leftW; outLeftH = leftH; outRightW = rightW; outRightH = rightH;
        }
        if (half) {
            outLeftW = HalfOf(outLeftW); outLeftH = HalfOf(outLeftH);
            outRightW = HalfOf(outRightW); outRightH = HalfOf(outRightH);
        }
        outW = outLeftW + outRightW;
        outH = std::max(outLeftH, outRightH);
        outFlags = (sym.valid ? (uint32_t)EYECAP_LIVE_OUT_SYMMETRIC : 0u) |
                   (reproj && head.Enabled() ? (uint32_t)EYECAP_LIVE_OUT_SMOOTHED : 0u) |
                   (half ? (uint32_t)EYECAP_LIVE_OUT_HALF : 0u) | (ovOn ? (uint32_t)EYECAP_LIVE_OUT_OVERLAYS : 0u);
        ovLayersIn = 0; ovNotReady = 0; ovFull = 0; ovSetupFailed = 0; ovBehind = 0;
        ovFrames = ovDraws = 0;
        ovLoggedFull = ovLoggedFirst = false;
        ovl.ResetMasks();
        for (auto& v : slotVal) v = 0;
        lastSlot = kLiveSlots - 1;
        nextDue = INT64_MIN;
        for (UINT i = 0; i < kRawSlots; ++i) { rawState[i] = kFree; rawDone[i] = 0; }
        haveItem = false;
        outCounter = 0;
        lastRawFree = 0;
        shared = 0; droppedBusy = 0; droppedGpu = 0; decimated = 0; superseded = 0; passBusy = 0;
        gameCalls = 0; gameSlow = 0; gameMax100ns = 0;
        smFrames = smGuarded = smUncovered = smCatch = smNoPose = smResets = 0;
        smCorrSum = smCorrMax = 0;
        stopWhy.clear();
        ready = false; done = false; failed = false; stage = false;
        liveSince = GetTickCount64();
        if (!stopEvt) stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        else ResetEvent(stopEvt);
        if (!workEvt) workEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        else ResetEvent(workEvt);
        const unsigned fps = shm->maxFps ? shm->maxFps : EYECAP_LIVE_DEFAULT_FPS;
        char what[256];
        if (reproj && sym.valid)
            snprintf(what, sizeof(what), "re-projected like the recording: symmetric fov %ux%u per eye (+-%.1f x +-%.1f deg%s), "
                     "head smoothing %s", sym.eyeW, sym.eyeH, std::atan(sym.tanH * (head.Enabled() ? head.fovScale : 1.0f)) * 57.29578,
                     std::atan(sym.tanV * (head.Enabled() ? head.fovScale : 1.0f)) * 57.29578,
                     head.Enabled() ? " after output_fov_scale" : "", head.Enabled() ? "on" : "off");
        else if (reproj)
            snprintf(what, sizeof(what), "re-projected like the recording: head smoothing on, each eye's own fov%s",
                     head.symmetricFov ? " (symmetric fov: no usable view fov at this frame)" : "");
        else if (mode == EYECAP_LIVE_REPROJ_RAW)
            snprintf(what, sizeof(what), "raw eyes (the viewer asked for them)");
        else
            snprintf(what, sizeof(what), "raw eyes (symmetric_fov %s, head_smoothing 0: same as a recording)",
                     head.symmetricFov ? "on but no usable view fov at this frame" : "0");
        const char* ovText = mode == EYECAP_LIVE_REPROJ_RAW ? "no overlays (raw eyes)"
                           : !ovCfg ? "no overlays (record_overlays = 0)"
                           : ovOn ? "overlays composited like a recording (record_overlays = 1)"
                                  : "overlays: none seen yet (record_overlays = 1: the first overlay layer starts a ring with them)";
        LOGI("live share: consumer found (pid %lu, wants %u fps, %s size) -- sharing this %s session: eyes %ux%u + %ux%u %s (%s); "
             "live view %s; %s; no dome, never the in-headset indicator", (unsigned long)shm->consumerPid, fps,
             half ? "half" : "full", GfxApiName(p.api), leftW, leftH, rightW, rightH, DxgiFormatName(fmt),
             srgb ? "sRGB-encoded" : "linear", what, ovText);
        worker = std::thread([this] { Run(); });
        st = St::Starting;
    }

    void Stop(const char* why) {
        stopWhy = why;
        st = St::Stopping;
        SetEvent(stopEvt);
    }

    // The worker has finished (failed setup or teardown done): join, give the block and the process slot back.
    void Finish(ULONGLONG now, bool wasFailure) {
        if (worker.joinable()) worker.join();
        CloseMap();
        void* me = this;
        g_owner.compare_exchange_strong(me, nullptr);
        st = St::Idle;
        nextProbe = now + (wasFailure ? 30000 : 1000);   // a failed setup is retried after 30 s, not every second
    }

    bool Throttled(int64_t ts) {   // the consumer's rate (the recorder's max_fps decimation)
        unsigned fps = shm->maxFps ? shm->maxFps : EYECAP_LIVE_DEFAULT_FPS;
        if (fps > 1000) fps = 1000;
        const int64_t period = 10000000LL / fps;
        if (nextDue != INT64_MIN && ts < nextDue - period / 4) { ++decimated; return true; }
        nextDue = (nextDue == INT64_MIN || ts > nextDue + period) ? ts + period : nextDue + period;
        return false;
    }

    void ClipToRing(const EyeView& L, const EyeView& R, EyeView& l, EyeView& r) const {
        l = L; r = R;   // clip to the ring (sizes are checked against the episode before this)
        if (l.w > leftW) l.w = leftW;
        if (l.h > H) l.h = H;
        if (r.w > W - leftW) r.w = W - leftW;
        if (r.h > H) r.h = H;
    }

    // Raw eyes at full size: copy straight into an exported slot and publish.
    void ShareFrame(const EyeView& L, const EyeView& R, int64_t ts) {
        if (Throttled(ts)) return;
        const UINT slot = PickOutSlot();
        if (slot == kLiveSlots) { ++droppedBusy; return; }
        EyeView l, r;
        ClipToRing(L, R, l, r);
        const uint64_t v = src->CopyEyes(slot, l, r, leftW, 0);
        if (!v) {   // D3D12: the previous copy into this slot is still on the GPU; nothing was submitted
            ++droppedGpu;
            if (slotVal[slot]) InterlockedExchange64(&shm->slotValue[slot], (int64_t)slotVal[slot]);
            return;
        }
        Publish(slot, v);
    }

    // The pass (re-projection / overlays / half size): copy into a raw slot (+ the overlay sub-images into its atlases) and
    // hand it to the worker (never waits for it).
    void ShareFrameStage(const EyeView& L, const EyeView& R, int64_t ts, const OverlayFrame* ov) {
        if (Throttled(ts)) return;
        UINT slot = kRawSlots;
        {
            std::lock_guard<std::mutex> lk(mx);
            for (UINT s = 0; s < kRawSlots && slot == kRawSlots; ++s) {
                if (rawState[s] == kFree || (rawState[s] == kDone && ring.free->GetCompletedValue() >= rawDone[s])) slot = s;
            }
            if (slot == kRawSlots && haveItem) {   // the worker is behind: the waiting frame is replaced by this one
                slot = mailbox.slot;
                haveItem = false;
                ++superseded;
            }
            if (slot != kRawSlots) rawState[slot] = kWriting;
        }
        if (slot == kRawSlots) { ++droppedBusy; return; }
        EyeView l, r;
        ClipToRing(L, R, l, r);
        // overlays: this frame's sub-images into the raw slot's atlases, in the same submission as the eye copies
        // (the recorder's packing; families whose atlas the worker has not made yet are requested and skipped)
        OverlayPacked pk;
        if (ovOn && ov && ov->n) {
            OverlayPack(*ov, ovl.ready.load(std::memory_order_acquire), ovl.failed.load(), pk, ovLoggedFull, "live share",
                        "not in the live view on such frames");
            ovLayersIn += pk.layersIn;
            ovNotReady += pk.notReady;
            ovFull += pk.full;
            ovSetupFailed += pk.setupFailed;
            ovBehind += pk.nBehind;
            if (pk.wantBits) ovl.want.fetch_or(pk.wantBits);
        }
        const uint64_t v = src->CopyEyes(slot, l, r, leftW, 0, pk.nCopy ? pk.copies : nullptr, pk.nCopy);
        if (!v) {   // D3D12: the previous copy into this slot is still on the GPU; nothing was submitted
            ++droppedGpu;
            std::lock_guard<std::mutex> lk(mx);
            rawState[slot] = kFree;
            return;
        }
        Item it{};
        it.slot = slot;
        it.ready = v;
        it.dispNs = ts * 100;
        it.nOv = pk.nDraw;
        it.nBehind = pk.nBehind;
        it.projUnpremul = ov && ov->projUnpremultiplied;
        for (uint32_t i = 0; i < pk.nDraw; ++i) it.ov[i] = pk.draws[i];
        const EyeView* ev[2] = {&l, &r};
        const UINT x0[2] = {0, leftW};
        for (int e = 0; e < 2; ++e) {
            ItemEye& ie = it.eye[e];
            const EyeView& vw = *ev[e];
            ie.valid = ReprojPoseUsable(vw);
            memcpy(ie.q, vw.orientation, sizeof(ie.q));
            memcpy(ie.p, vw.position, sizeof(ie.p));
            memcpy(ie.fov, vw.fov, sizeof(ie.fov));
            ie.x0 = x0[e]; ie.w = vw.w; ie.h = vw.h;
        }
        {
            std::lock_guard<std::mutex> lk(mx);
            if (haveItem) {   // a frame the worker has not taken yet: replaced (its slot was never read)
                rawState[mailbox.slot] = kFree;
                ++superseded;
            }
            mailbox = it;
            haveItem = true;
            rawState[slot] = kPending;
        }
        SetEvent(workEvt);
    }

    bool WantsOverlays() const {   // packing (ovOn with the pass) or watching for the first overlay layer (ovOn off)
        return st == St::Live && mode == EYECAP_LIVE_REPROJ_CONFIG && ovCfg && (!ovOn || stage.load(std::memory_order_relaxed));
    }

    bool Active() const {
        return st == St::Live && GetTickCount64() - lastPublish.load(std::memory_order_relaxed) < 1000;
    }

    void Frame(const EyeView& L, const EyeView& R, DXGI_FORMAT stageFmt, bool srgbEnc, int64_t ts, const HeadSmoothingSettings& h,
               bool overlaysOn, const OverlayFrame* ov) {
        if (p.api != GfxApi::D3D11 && p.api != GfxApi::D3D12) return;
        if (overlaysOn && ov && ov->n) ovSeen = true;   // also from a recording's collection: the next ring has them at once
        const ULONGLONG now = GetTickCount64();
        switch (st) {
        case St::Idle: {
            if (now < nextProbe) return;
            nextProbe = now + 1000;
            if (!Probe(now)) return;
            void* expected = nullptr;
            if (!g_owner.compare_exchange_strong(expected, this)) { CloseMap(); return; }   // another session shares
            if (stageFmt == DXGI_FORMAT_UNKNOWN || !L.w || !L.h || !R.w || !R.h) {
                if (!loggedFormat) {
                    loggedFormat = true;
                    LOGW("live share: a consumer is watching, but this swapchain format cannot be shared -- not sharing");
                }
                CloseMap();
                g_owner.store(nullptr);
                return;
            }
            if (!Claim(now)) {   // another game claimed the block a moment ago
                if (!loggedOther) {
                    loggedOther = true;
                    LOGI("live share: a consumer is watching, but game pid %lu took the block first -- not sharing this one",
                         (unsigned long)shm->producerPid);
                }
                CloseMap();
                g_owner.store(nullptr);
                return;
            }
            Start(L, R, stageFmt, srgbEnc, h, overlaysOn);
            return;
        }
        case St::Starting:
            if (done.load(std::memory_order_acquire)) { Finish(now, true); return; }   // setup failed (logged)
            if (!Owner()) { Stop("another game took the block over"); return; }
            shm->producerHeartbeat = now;   // claimed: a second game's Probe must see this producer while the worker sets up
            if (!ready.load(std::memory_order_acquire)) return;
            st = St::Live;
            [[fallthrough]];
        case St::Live: {
            if (!Owner()) { Stop("another game took the block over"); return; }
            shm->producerHeartbeat = now;
            if (!EyeCapLiveFresh(shm->consumerHeartbeat, now)) { Stop("consumer gone: no heartbeat for 2 s"); return; }
            if (L.w != leftW || L.h != leftH || R.w != rightW || R.h != rightH || stageFmt != fmt || srgbEnc != srgb) {
                Stop("eye size / format changed -- a new ring follows");
                return;
            }
            const uint32_t want = shm->reproject == EYECAP_LIVE_REPROJ_RAW ? (uint32_t)EYECAP_LIVE_REPROJ_RAW : (uint32_t)EYECAP_LIVE_REPROJ_CONFIG;
            if (want != mode) {
                Stop(want == EYECAP_LIVE_REPROJ_RAW ? "the viewer switched to raw eyes -- a new ring follows"
                                                    : "the viewer switched to the re-projection -- a new ring follows");
                return;
            }
            if (half != (shm->size == EYECAP_LIVE_SIZE_HALF)) {
                Stop(half ? "the viewer switched to full size -- a new ring follows"
                          : "the viewer switched to half size -- a new ring follows");
                return;
            }
            if (mode == EYECAP_LIVE_REPROJ_CONFIG && !SameHead(h, head)) {
                Stop("re-projection keys changed by a config reload -- a new ring follows");
                return;
            }
            if (mode == EYECAP_LIVE_REPROJ_CONFIG && overlaysOn != ovCfg) {
                Stop("record_overlays changed by a config reload -- a new ring follows");
                return;
            }
            if (mode == EYECAP_LIVE_REPROJ_CONFIG && ovCfg && !ovOn && ovSeen) {
                Stop("the game shows overlay layers -- a new ring follows with them composited");
                return;
            }
            const int64_t c0 = QpcNow100ns();
            if (stage.load(std::memory_order_relaxed)) ShareFrameStage(L, R, ts, ov);
            else ShareFrame(L, R, ts);
            const int64_t dt = QpcNow100ns() - c0;
            ++gameCalls;
            if (dt > gameMax100ns.load(std::memory_order_relaxed)) gameMax100ns.store(dt, std::memory_order_relaxed);
            if (dt > kSlowCall100ns) ++gameSlow;
            return;
        }
        case St::Stopping:
            if (done.load(std::memory_order_acquire)) Finish(now, false);
            return;
        }
    }

    ~Impl() {
        if (worker.joinable()) {
            if (st != St::Stopping) stopWhy = "session ended";
            quickTeardown.store(true);   // the session is going away: no game frame to protect, free without pauses
            SetEvent(stopEvt);
            worker.join();
        }
        CloseMap();
        void* me = this;
        g_owner.compare_exchange_strong(me, nullptr);
        if (stopEvt) CloseHandle(stopEvt);
        if (workEvt) CloseHandle(workEvt);
    }
};

LiveShare::LiveShare(const LiveShareParams& p) : impl_(std::make_unique<Impl>(p)) {}
LiveShare::~LiveShare() = default;

void LiveShare::Frame(const EyeView& L, const EyeView& R, DXGI_FORMAT stage, bool srgb, int64_t ts100ns,
                      const HeadSmoothingSettings& head, bool overlaysOn, const OverlayFrame* ov) {
    impl_->Frame(L, R, stage, srgb, ts100ns, head, overlaysOn, ov);
}

bool LiveShare::WantsOverlays() const { return impl_->WantsOverlays(); }
bool LiveShare::Active() const { return impl_->Active(); }
