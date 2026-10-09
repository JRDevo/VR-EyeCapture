// recorder -- see recorder.h.
#include <initguid.h>   // CODECAPI_* GUIDs (selectany, no clash with mfuuid / strmiids)
#include "recorder.h"
#include <codecapi.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <algorithm>
#include <vector>
#include "audio_capture.h"
#include "log.h"
#include "st3d.h"

namespace {

const UINT kRingSize = 3;   // shared SBS slots (game thread never waits; full ring = dropped frame)
const UINT kNv12Pool = 4;   // NV12 surfaces in flight inside the encoder

// Linear swapchain formats (UNORM / R10G10B10A2 / FP16 without _SRGB) hold linear light per the OpenXR spec. The D3D11
// video processor accepts a G10 input colour space but (measured on NVIDIA) does not apply the curve, so linear input
// is sRGB-encoded here first, on the private encoder device (no game state is touched).
const char kLinearToSrgbCs[] =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<unorm float4> dst : register(u0);\n"
    "float3 Enc(float3 l) {\n"
    "    l = saturate(l);\n"
    "    return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    dst[id.xy] = float4(Enc(src[id.xy].rgb), 1.0);\n"
    "}\n";

// ---- NV12 pool: tracked samples return their surface here when Media Foundation releases them ----
struct Nv12Pool {
    std::mutex m;
    std::condition_variable cv;
    std::vector<bool> busy;
    int Acquire(DWORD timeoutMs) {
        std::unique_lock<std::mutex> lk(m);
        int idx = -1;
        cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
            for (size_t i = 0; i < busy.size(); ++i) if (!busy[i]) { idx = (int)i; return true; }
            return false;
        });
        if (idx >= 0) busy[(size_t)idx] = true;
        return idx;
    }
    void Release(int i) {
        { std::lock_guard<std::mutex> lk(m); if (i >= 0 && (size_t)i < busy.size()) busy[(size_t)i] = false; }
        cv.notify_all();
    }
    bool AllFree(DWORD timeoutMs) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
            for (bool b : busy) if (b) return false;
            return true;
        });
    }
};

class PoolReturn final : public IMFAsyncCallback {
public:
    PoolReturn(std::shared_ptr<Nv12Pool> p, int i) : pool_(std::move(p)), idx_(i) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) { *ppv = static_cast<IMFAsyncCallback*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { const ULONG r = --ref_; if (!r) delete this; return r; }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult*) override { pool_->Release(idx_); return S_OK; }
private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<Nv12Pool> pool_;
    int idx_;
};

std::string GuidCategoryName(const GUID& g) {
    if (g == MFT_CATEGORY_VIDEO_ENCODER) return "video encoder";
    if (g == MFT_CATEGORY_VIDEO_PROCESSOR) return "video processor";
    if (g == MFT_CATEGORY_AUDIO_ENCODER) return "audio encoder";
    if (g == MFT_CATEGORY_AUDIO_EFFECT) return "audio effect";
    return "other";
}

std::string HrText(HRESULT hr) {
    char b[32];
    snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}

// Highest AAC bitrate (bytes/s) the system AAC encoder accepts for 48 kHz stereo; 0 = no AAC encoder.
UINT32 ProbeAacBytesPerSecond() {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         nullptr, &outInfo, &acts, &n)) || n == 0)
        return 0;
    UINT32 best = 0;
    ComPtr<IMFTransform> t;
    if (SUCCEEDED(acts[0]->ActivateObject(IID_PPV_ARGS(&t)))) {
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(t->SetInputType(0, in.Get(), 0))) {
            for (UINT32 bps : {32000u, 24000u, 20000u, 16000u}) {
                ComPtr<IMFMediaType> out;
                MFCreateMediaType(&out);
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bps);
                if (SUCCEEDED(t->SetOutputType(0, out.Get(), MFT_SET_TYPE_TEST_ONLY))) { best = bps; break; }
            }
        }
        acts[0]->ShutdownObject();
    }
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return best;
}

} // namespace

const char* RecStateName(RecState s) {
    switch (s) {
    case RecState::Idle:      return "idle";
    case RecState::Starting:  return "starting";
    case RecState::Recording: return "recording";
    case RecState::Stopping:  return "stopping";
    default:                  return "failed";
    }
}

struct Recorder::Impl {
    Recorder* owner = nullptr;
    std::thread worker;

    // this recording
    IFrameSource* src = nullptr;
    RecorderSettings s;
    UINT W = 0, H = 0, leftW = 0, outW = 0, outH = 0, fps = 90;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool srgb = true;
    bool hevc = true;
    UINT bitrate = 0;

    // private encoder device (kept across recordings while the adapter is the same)
    ComPtr<ID3D11Device> enc;
    LUID encLuid{};
    ComPtr<ID3D11DeviceContext> encCtx;
    ComPtr<ID3D11DeviceContext4> encCtx4;
    SharedRing ring;
    bool ringCreated = false;

    // CPU side of the ring
    struct Item { UINT slot; uint64_t ready; int64_t ts; };
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Item> queue;
    std::vector<bool> slotBusy;
    std::vector<uint64_t> slotFreeValue;
    std::atomic<int> gameBusy{0};
    std::atomic<bool> stopReq{false};
    uint64_t freeCounter = 0;

    // video processor
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    std::vector<ComPtr<ID3D11VideoProcessorInputView>> inViews;
    std::vector<ComPtr<ID3D11Texture2D>> localCopies;   // only if the VP cannot read the shared textures directly
    // linear -> sRGB pre-pass (linear swapchain formats only)
    ComPtr<ID3D11ComputeShader> linCs;
    std::vector<ComPtr<ID3D11ShaderResourceView>> linSrv;
    ComPtr<ID3D11Texture2D> linTex;
    ComPtr<ID3D11UnorderedAccessView> linUav;
    ComPtr<ID3D11VideoProcessorInputView> linView;
    HANDLE devHandle = nullptr;   // IMFDXGIDeviceManager handle, to lock the device around our GPU work
    std::vector<ComPtr<ID3D11Texture2D>> nv12;
    std::vector<ComPtr<ID3D11VideoProcessorOutputView>> outViews;
    std::vector<ComPtr<IMFAsyncCallback>> poolCb;
    std::shared_ptr<Nv12Pool> pool;

    // writer
    ComPtr<IMFDXGIDeviceManager> devMgr;
    UINT resetToken = 0;
    std::mutex wm;
    ComPtr<IMFSinkWriter> writer;
    DWORD vStream = 0, aStream = 0;
    bool hasAudio = false;
    UINT32 aacBytes = 0;
    AudioCapture audio;

    // timing (t0 = first video frame on the QPC clock)
    std::atomic<int64_t> t0{INT64_MIN};
    int64_t lastTs = INT64_MIN;      // game thread
    int64_t nextDue = INT64_MIN;     // game thread (max_fps)
    std::atomic<int64_t> lastWrittenTs{0};

    // stats
    std::atomic<uint64_t> written{0}, dropRing{0}, dropGpu{0}, dropPool{0}, dropDup{0}, decimated{0}, audioFrames{0};
    std::atomic<uint64_t> writeErrors{0};
    std::mutex statusLock;
    std::string message;
    ULONGLONG startTick = 0;

    void Run();
    bool Init();
    bool InitVideoProcessor();
    bool InitLinearPass();
    HRESULT CreateWriter(bool useHevc, bool hardware, std::string& why);
    void LogTransforms();
    void Process(const Item& it);
    void FreeSlot(UINT slot, uint64_t freeValue, bool updateValue);
    void OnAudio(const int16_t* pcm, uint32_t frames, int64_t startQpc);
    void Cleanup();
    void SetMessage(const std::string& m) { std::lock_guard<std::mutex> lk(statusLock); message = m; }
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }

Recorder::~Recorder() {
    Stop();
    WaitIdle(30000);
    if (impl_->worker.joinable()) impl_->worker.detach();   // only if finalize hung for 30 s
}

bool Recorder::Start(IFrameSource* src, const RecorderSettings& s, UINT w, UINT h, UINT leftWidth, DXGI_FORMAT fmt, bool srgbEncoded) {
    const RecState st = state_.load();
    if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    Impl& m = *impl_;
    m.src = src;
    m.s = s;
    m.W = w; m.H = h; m.leftW = leftWidth; m.fmt = fmt; m.srgb = srgbEncoded;
    m.fps = s.fps ? s.fps : 90;
    if (s.maxFps && s.maxFps < m.fps) m.fps = s.maxFps;
    m.hevc = s.hevc;
    m.stopReq = false;
    m.t0 = INT64_MIN;
    m.lastTs = INT64_MIN;
    m.nextDue = INT64_MIN;
    m.lastWrittenTs = 0;
    m.written = 0; m.dropRing = 0; m.dropGpu = 0; m.dropPool = 0; m.dropDup = 0; m.decimated = 0; m.audioFrames = 0; m.writeErrors = 0;
    m.queue.clear();
    m.SetMessage("");
    m.startTick = GetTickCount64();
    state_ = RecState::Starting;
    m.worker = std::thread([this] { impl_->Run(); });
    return true;
}

bool Recorder::SubmitFrame(int64_t ts, const EyeView& L, const EyeView& R) {
    Impl& m = *impl_;
    m.gameBusy.fetch_add(1);
    struct Guard { std::atomic<int>& c; ~Guard() { c.fetch_sub(1); } } guard{m.gameBusy};
    if (state_.load() != RecState::Recording) return false;
    if (m.lastTs != INT64_MIN && ts <= m.lastTs) { ++m.dropDup; return false; }
    if (m.s.maxFps) {
        const int64_t period = 10000000LL / m.s.maxFps;
        if (m.nextDue != INT64_MIN && ts < m.nextDue - period / 4) { ++m.decimated; return false; }
        m.nextDue = (m.nextDue == INT64_MIN || ts > m.nextDue + period) ? ts + period : m.nextDue + period;
    }
    UINT slot = 0;
    uint64_t waitFree = 0;
    {
        std::lock_guard<std::mutex> lk(m.qm);
        size_t i = 0;
        for (; i < m.slotBusy.size(); ++i) if (!m.slotBusy[i]) break;
        if (i == m.slotBusy.size()) { ++m.dropRing; return false; }
        m.slotBusy[i] = true;
        slot = (UINT)i;
        waitFree = m.slotFreeValue[i];
    }
    EyeView l = L, r = R;
    l.w = std::min<uint32_t>(l.w, m.leftW);
    l.h = std::min<uint32_t>(l.h, m.H);
    r.w = std::min<uint32_t>(r.w, m.W - m.leftW);
    r.h = std::min<uint32_t>(r.h, m.H);
    const uint64_t ready = m.src->CopyEyes(slot, l, r, m.leftW, waitFree);
    if (!ready) {
        m.FreeSlot(slot, 0, false);
        ++m.dropGpu;
        return false;
    }
    int64_t expected = INT64_MIN;
    m.t0.compare_exchange_strong(expected, ts);
    m.lastTs = ts;
    {
        std::lock_guard<std::mutex> lk(m.qm);
        m.queue.push_back({slot, ready, ts});
    }
    m.qcv.notify_one();
    return true;
}

void Recorder::Stop() {
    Impl& m = *impl_;
    m.stopReq = true;
    RecState e = RecState::Recording;
    if (!state_.compare_exchange_strong(e, RecState::Stopping)) {
        e = RecState::Starting;
        state_.compare_exchange_strong(e, RecState::Stopping);
    }
    m.qcv.notify_all();
}

bool Recorder::WaitIdle(DWORD ms) {
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const RecState st = state_.load();
        if (st == RecState::Idle || st == RecState::Failed) break;
        if (GetTickCount64() - t0 >= ms) return false;
        Sleep(5);
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    return true;
}

RecorderStatus Recorder::Status() {
    Impl& m = *impl_;
    RecorderStatus st;
    st.state = state_.load();
    st.width = m.outW; st.height = m.outH; st.fps = m.fps;
    st.framesWritten = m.written.load();
    st.framesDropped = m.dropRing.load() + m.dropGpu.load() + m.dropPool.load();
    const int64_t t0 = m.t0.load();
    st.elapsedSec = (t0 != INT64_MIN) ? (double)(m.lastWrittenTs.load()) / 1e7 : 0.0;
    st.file = m.s.path;
    std::lock_guard<std::mutex> lk(m.statusLock);
    st.message = m.message;
    return st;
}

// ------------------------------------------------------------------------------------------------------------------

void Recorder::Impl::FreeSlot(UINT slot, uint64_t freeValue, bool updateValue) {
    std::lock_guard<std::mutex> lk(qm);
    if (slot < slotBusy.size()) {
        slotBusy[slot] = false;
        if (updateValue) slotFreeValue[slot] = freeValue;
    }
}

void Recorder::Impl::Run() {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    const std::string file = elog::Narrow(s.path);
    bool ok = SUCCEEDED(mfHr) && Init();
    if (!ok) {
        LOGE("recorder: start FAILED for %s (%s) -- see the lines above", file.c_str(), message.c_str());
        Cleanup();
        DeleteFileW(s.path.c_str());
        if (SUCCEEDED(mfHr)) MFShutdown();
        if (SUCCEEDED(coHr)) CoUninitialize();
        owner->state_ = RecState::Failed;
        return;
    }
    RecState e = RecState::Starting;
    owner->state_.compare_exchange_strong(e, RecState::Recording);
    LOGI("recorder: RECORDING -> %s", file.c_str());

    // main loop
    for (;;) {
        Item it{};
        {
            std::unique_lock<std::mutex> lk(qm);
            qcv.wait(lk, [&] { return !queue.empty() || stopReq.load(); });
            if (queue.empty()) break;
            it = queue.front();
            queue.pop_front();
        }
        Process(it);
    }
    // stop: let the game thread leave SubmitFrame, then drain what it queued
    owner->state_ = RecState::Stopping;
    {
        const ULONGLONG t = GetTickCount64();
        while (gameBusy.load() > 0 && GetTickCount64() - t < 2000) Sleep(1);
    }
    for (;;) {
        Item it{};
        {
            std::lock_guard<std::mutex> lk(qm);
            if (queue.empty()) break;
            it = queue.front();
            queue.pop_front();
        }
        Process(it);
    }
    if (hasAudio) audio.Stop();

    HRESULT fhr = S_OK;
    {
        std::lock_guard<std::mutex> lk(wm);
        if (writer) fhr = writer->Finalize();
        writer.Reset();
    }
    if (pool) pool->AllFree(5000);
    Cleanup();
    MFShutdown();
    if (SUCCEEDED(coHr)) CoUninitialize();

    const double dur = (double)lastWrittenTs.load() / 1e7;
    const uint64_t frames = written.load();
    std::string st3dReport = "st3d: skipped";
    bool fileOk = SUCCEEDED(fhr) && frames > 0;
    if (fileOk) {
        PatchSt3d(s.path, 2 /*left-right*/, st3dReport);
    } else {
        DeleteFileW(s.path.c_str());
    }
    char sum[700];
    snprintf(sum, sizeof(sum),
             "recording summary: %s | %s | %ux%u %s @%u fps nominal | frames written %llu, dropped %llu "
             "(ring full %llu, gpu busy %llu, encoder stall %llu), duplicates %llu, over max_fps %llu | duration %.2f s | "
             "audio %s (%llu frames = %.2f s) | finalize %s | %s",
             fileOk ? "OK" : "FAILED", file.c_str(), outW, outH, hevc ? "HEVC" : "H.264", fps,
             (unsigned long long)frames,
             (unsigned long long)(dropRing.load() + dropGpu.load() + dropPool.load()),
             (unsigned long long)dropRing.load(), (unsigned long long)dropGpu.load(), (unsigned long long)dropPool.load(),
             (unsigned long long)dropDup.load(), (unsigned long long)decimated.load(), dur,
             hasAudio ? "AAC" : "none", (unsigned long long)audioFrames.load(), audioFrames.load() / 48000.0,
             HrText(fhr).c_str(), st3dReport.c_str());
    if (fileOk) LOGI("%s", sum); else LOGE("%s", sum);
    SetMessage(fileOk ? std::string("saved ") + file : std::string("recording failed (no frames or finalize error)"));
    owner->state_ = RecState::Idle;
}

bool Recorder::Impl::Init() {
    // --- encoder device on the game's adapter ---
    const LUID luid = src->AdapterLuid();
    if (!enc || encLuid.LowPart != luid.LowPart || encLuid.HighPart != luid.HighPart) {
        std::string name;
        enc = CreateEncoderDevice(luid, &name);
        if (!enc) { SetMessage("no encoder device"); return false; }
        encLuid = luid;
        encCtx.Reset(); encCtx4.Reset(); devMgr.Reset();
        enc->GetImmediateContext(&encCtx);
        encCtx.As(&encCtx4);
    }
    if (!encCtx4) { SetMessage("no ID3D11DeviceContext4"); return false; }

    // --- output size ---
    outW = W; outH = H;
    auto scaleTo = [&](UINT maxW, UINT maxH) {
        if (outW > maxW) { outH = (UINT)((uint64_t)outH * maxW / outW); outW = maxW; }
        if (outH > maxH) { outW = (UINT)((uint64_t)outW * maxH / outH); outH = maxH; }
    };
    if (s.maxWidth) scaleTo(s.maxWidth, 16384);
    const UINT codecMax = hevc ? 8192 : 4096;
    if (outW > codecMax || outH > codecMax) {
        LOGW("recorder: %ux%u exceeds the %s limit of %u -- scaling down (set max_width or use codec = hevc)", outW, outH,
             hevc ? "HEVC" : "H.264", codecMax);
        scaleTo(codecMax, codecMax);
    }
    outW &= ~1u; outH &= ~1u;
    if (outW < 64 || outH < 64) { SetMessage("frame too small"); return false; }
    bitrate = s.bitrate ? s.bitrate : (hevc ? 120000000u : 80000000u);

    LOGI("recorder: init %s -- SBS %ux%u (left %u) %s %s -> %ux%u %s %.0f Mb/s, %u fps nominal, GOP %u",
         GfxApiName(src->Api()), W, H, leftW, DxgiFormatName(fmt), srgb ? "sRGB-encoded" : "linear", outW, outH,
         hevc ? "HEVC" : "H.264", bitrate / 1e6, fps, fps * 2);

    // --- shared ring ---
    if (!src->CreateRing(enc.Get(), W, H, fmt, kRingSize, ring)) { SetMessage("shared ring creation failed"); return false; }
    ringCreated = true;
    {
        std::lock_guard<std::mutex> lk(qm);
        slotBusy.assign(kRingSize, false);
        slotFreeValue.assign(kRingSize, 0);
    }
    freeCounter = 0;

    if (!InitVideoProcessor()) return false;

    // --- audio (before the writer: the stream must exist before BeginWriting) ---
    // Capture starts first (OnAudio drops everything until the writer exists and video has started); if it cannot
    // start, the file gets no audio stream at all rather than an empty one that would stall the muxer.
    hasAudio = false;
    if (s.audio) {
        aacBytes = ProbeAacBytesPerSecond();
        if (!aacBytes) LOGW("audio: no AAC encoder found -- recording without audio");
        else if (!audio.Start(s.audioDevice, [this](const int16_t* p, uint32_t n, int64_t t) { OnAudio(p, n, t); }))
            LOGW("audio: loopback capture failed to start -- recording without audio");
        else hasAudio = true;
    }

    // --- sink writer (attempts) ---
    if (!devMgr) {
        HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &devMgr);
        if (SUCCEEDED(hr)) hr = devMgr->ResetDevice(enc.Get(), resetToken);
        if (FAILED(hr)) { LOGE("recorder: IMFDXGIDeviceManager failed hr=0x%08lx", (unsigned long)hr); SetMessage("DXGI device manager failed"); return false; }
    }
    struct Attempt { bool hevc; bool hw; };
    std::vector<Attempt> attempts = {{hevc, true}};
    if (hevc && outW <= 4096 && outH <= 4096) attempts.push_back({false, true});
    attempts.push_back({hevc, false});
    if (hevc && outW <= 4096 && outH <= 4096) attempts.push_back({false, false});
    HRESULT hr = E_FAIL;
    for (const Attempt& a : attempts) {
        std::string why;
        if (a.hevc != hevc) { bitrate = s.bitrate ? s.bitrate : 80000000u; }
        hr = CreateWriter(a.hevc, a.hw, why);
        if (SUCCEEDED(hr)) {
            if (a.hevc != hevc) LOGW("recorder: HEVC unavailable -- fell back to H.264");
            hevc = a.hevc;
            break;
        }
        LOGW("recorder: sink writer attempt %s %s failed at %s hr=0x%08lx", a.hevc ? "HEVC" : "H.264",
             a.hw ? "hardware" : "software", why.c_str(), (unsigned long)hr);
        DeleteFileW(s.path.c_str());
    }
    if (FAILED(hr)) { SetMessage("no usable video encoder"); return false; }
    LogTransforms();
    if (FAILED(devMgr->OpenDeviceHandle(&devHandle))) devHandle = nullptr;
    if (stopReq.load()) LOGW("recorder: stop requested during start");
    return true;
}

bool Recorder::Impl::InitVideoProcessor() {
    HRESULT hr = enc.As(&vdev);
    if (SUCCEEDED(hr)) hr = encCtx.As(&vctx);
    if (FAILED(hr)) { LOGE("recorder: encoder device has no video interfaces hr=0x%08lx", (unsigned long)hr); SetMessage("no video device"); return false; }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {fps, 1};
    cd.InputWidth = W; cd.InputHeight = H;
    cd.OutputFrameRate = {fps, 1};
    cd.OutputWidth = outW; cd.OutputHeight = outH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    hr = vdev->CreateVideoProcessorEnumerator(&cd, &venum);
    if (FAILED(hr)) { LOGE("recorder: CreateVideoProcessorEnumerator hr=0x%08lx", (unsigned long)hr); SetMessage("video processor"); return false; }
    const DXGI_FORMAT vpFmt = srgb ? fmt : DXGI_FORMAT_R8G8B8A8_UNORM;   // linear input goes through the pre-pass
    UINT inSup = 0, outSup = 0;
    venum->CheckVideoProcessorFormat(vpFmt, &inSup);
    venum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outSup);
    if (!(inSup & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(outSup & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        LOGE("recorder: video processor cannot convert %s -> NV12 (in 0x%x out 0x%x)", DxgiFormatName(vpFmt), inSup, outSup);
        SetMessage("format not supported by the video processor");
        return false;
    }
    hr = vdev->CreateVideoProcessor(venum.Get(), 0, &vp);
    if (FAILED(hr)) { LOGE("recorder: CreateVideoProcessor hr=0x%08lx", (unsigned long)hr); SetMessage("video processor"); return false; }

    // Colour: input = full-range sRGB-encoded RGB (linear formats are encoded by the pre-pass), output = BT.709 limited.
    const DXGI_COLOR_SPACE_TYPE inCs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const DXGI_COLOR_SPACE_TYPE outCs = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    ComPtr<ID3D11VideoContext1> vctx1;
    ComPtr<ID3D11VideoProcessorEnumerator1> venum1;
    bool cs1 = SUCCEEDED(vctx.As(&vctx1));
    if (cs1 && SUCCEEDED(venum.As(&venum1))) {
        BOOL sup = FALSE;
        if (SUCCEEDED(venum1->CheckVideoProcessorFormatConversion(vpFmt, inCs, DXGI_FORMAT_NV12, outCs, &sup)) && !sup)
            LOGW("recorder: the video processor reports no %s RGB -> NV12 BT.709 conversion; colours may be off", DxgiFormatName(vpFmt));
    }
    if (cs1) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, inCs);
        vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), outCs);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.RGB_Range = 0;          // full
        in.YCbCr_Matrix = 1;       // 709
        out.RGB_Range = 0;
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in);
        vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &out);
    }
    const RECT srcR{0, 0, (LONG)W, (LONG)H}, dstR{0, 0, (LONG)outW, (LONG)outH};
    vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &srcR);
    vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dstR);
    vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dstR);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    vctx->VideoProcessorSetStreamAlpha(vp.Get(), 0, FALSE, 1.0f);
    D3D11_VIDEO_COLOR bg{};
    bg.RGBA.A = 1.0f;
    vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), FALSE, &bg);
    vctx->VideoProcessorSetOutputAlphaFillMode(vp.Get(), D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);

    // input views (directly on the shared ring; local copies if the driver refuses)
    inViews.clear();
    localCopies.clear();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (!srgb) {
        if (!InitLinearPass()) return false;
        hr = vdev->CreateVideoProcessorInputView(linTex.Get(), venum.Get(), &iv, &linView);
        if (FAILED(hr)) { LOGE("recorder: input view on the linear pre-pass texture hr=0x%08lx", (unsigned long)hr); SetMessage("video processor input view"); return false; }
    }
    bool direct = true;
    for (auto& t : ring.tex) {
        if (!srgb) break;
        ComPtr<ID3D11VideoProcessorInputView> v;
        if (FAILED(vdev->CreateVideoProcessorInputView(t.Get(), venum.Get(), &iv, &v))) { direct = false; break; }
        inViews.push_back(v);
    }
    if (srgb && !direct) {
        LOGW("recorder: video processor cannot read the shared ring directly -- adding one GPU copy per frame");
        inViews.clear();
        for (size_t i = 0; i < ring.tex.size(); ++i) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> t;
            ComPtr<ID3D11VideoProcessorInputView> v;
            if (FAILED(enc->CreateTexture2D(&d, nullptr, &t)) ||
                FAILED(hr = vdev->CreateVideoProcessorInputView(t.Get(), venum.Get(), &iv, &v))) {
                LOGE("recorder: CreateVideoProcessorInputView hr=0x%08lx", (unsigned long)hr);
                SetMessage("video processor input view");
                return false;
            }
            localCopies.push_back(t);
            inViews.push_back(v);
        }
    }

    // NV12 pool
    nv12.clear(); outViews.clear(); poolCb.clear();
    pool = std::make_shared<Nv12Pool>();
    pool->busy.assign(kNv12Pool, false);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < kNv12Pool; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = outW; d.Height = outH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        ComPtr<ID3D11Texture2D> t;
        hr = enc->CreateTexture2D(&d, nullptr, &t);
        if (FAILED(hr)) {
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = enc->CreateTexture2D(&d, nullptr, &t);
        }
        ComPtr<ID3D11VideoProcessorOutputView> v;
        if (SUCCEEDED(hr)) hr = vdev->CreateVideoProcessorOutputView(t.Get(), venum.Get(), &ov, &v);
        if (FAILED(hr)) { LOGE("recorder: NV12 surface %ux%u failed hr=0x%08lx", outW, outH, (unsigned long)hr); SetMessage("NV12 surface"); return false; }
        nv12.push_back(t);
        outViews.push_back(v);
        ComPtr<IMFAsyncCallback> cb;
        cb.Attach(new PoolReturn(pool, (int)i));
        poolCb.push_back(cb);
    }
    return true;
}

bool Recorder::Impl::InitLinearPass() {
    ComPtr<ID3DBlob> code, errs;
    HRESULT hr = D3DCompile(kLinearToSrgbCs, sizeof(kLinearToSrgbCs) - 1, "linear_to_srgb", nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
    if (FAILED(hr)) {
        LOGE("recorder: linear->sRGB shader compile failed hr=0x%08lx %s", (unsigned long)hr,
             errs ? (const char*)errs->GetBufferPointer() : "");
        SetMessage("shader compile");
        return false;
    }
    hr = enc->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &linCs);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = enc->CreateTexture2D(&d, nullptr, &linTex);
    if (SUCCEEDED(hr)) hr = enc->CreateUnorderedAccessView(linTex.Get(), nullptr, &linUav);
    linSrv.clear();
    for (size_t i = 0; SUCCEEDED(hr) && i < ring.tex.size(); ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> v;
        hr = enc->CreateShaderResourceView(ring.tex[i].Get(), &sd, &v);
        linSrv.push_back(v);
    }
    if (FAILED(hr)) { LOGE("recorder: linear->sRGB pre-pass setup failed hr=0x%08lx", (unsigned long)hr); SetMessage("linear pre-pass"); return false; }
    LOGI("recorder: linear swapchain format -> sRGB-encoding pre-pass (compute) on the encoder device");
    return true;
}

HRESULT Recorder::Impl::CreateWriter(bool useHevc, bool hardware, std::string& why) {
    std::lock_guard<std::mutex> lk(wm);
    writer.Reset();
    DeleteFileW(s.path.c_str());
    ComPtr<IMFAttributes> a;
    HRESULT hr = MFCreateAttributes(&a, 6);
    if (FAILED(hr)) { why = "attributes"; return hr; }
    a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
    a->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, devMgr.Get());
    a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> w;
    hr = MFCreateSinkWriterFromURL(s.path.c_str(), nullptr, a.Get(), &w);
    if (FAILED(hr)) { why = "MFCreateSinkWriterFromURL (output folder writable?)"; return hr; }

    auto setColour = [](IMFMediaType* t) {
        t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    };
    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, useHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, outW, outH);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, useHevc ? (UINT32)eAVEncH265VProfile_Main_420_8 : (UINT32)eAVEncH264VProfile_High);
    setColour(out.Get());
    hr = w->AddStream(out.Get(), &vStream);
    if (FAILED(hr)) { why = "AddStream(video)"; return hr; }

    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, outW, outH);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    setColour(in.Get());

    ComPtr<IMFAttributes> enc;
    MFCreateAttributes(&enc, 4);
    enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
    enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
    enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, fps * 2);
    hr = w->SetInputMediaType(vStream, in.Get(), enc.Get());
    if (FAILED(hr)) {
        LOGW("recorder: encoder rejected the VBR/GOP parameters (hr=0x%08lx) -- using its defaults", (unsigned long)hr);
        hr = w->SetInputMediaType(vStream, in.Get(), nullptr);
    }
    if (FAILED(hr)) { why = "SetInputMediaType(video NV12)"; return hr; }

    if (hasAudio) {
        ComPtr<IMFMediaType> ao, ai;
        MFCreateMediaType(&ao);
        ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aacBytes);
        hr = w->AddStream(ao.Get(), &aStream);
        if (FAILED(hr)) { why = "AddStream(audio)"; return hr; }
        MFCreateMediaType(&ai);
        ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        hr = w->SetInputMediaType(aStream, ai.Get(), nullptr);
        if (FAILED(hr)) { why = "SetInputMediaType(audio PCM)"; return hr; }
    }
    hr = w->BeginWriting();
    if (FAILED(hr)) { why = "BeginWriting"; return hr; }
    writer = w;
    LOGI("recorder: sink writer ready (%s, %s transforms%s)", useHevc ? "HEVC" : "H.264", hardware ? "hardware" : "software",
         hasAudio ? (aacBytes >= 32000 ? ", AAC 256 kb/s" : aacBytes >= 24000 ? ", AAC 192 kb/s" : ", AAC <192 kb/s") : ", no audio");
    return S_OK;
}

void Recorder::Impl::LogTransforms() {
    ComPtr<IMFSinkWriterEx> ex;
    if (!writer || FAILED(writer.As(&ex))) return;
    for (DWORD stream : {vStream, aStream}) {
        if (stream == aStream && !hasAudio) continue;
        for (DWORD i = 0; i < 8; ++i) {
            GUID cat{};
            ComPtr<IMFTransform> t;
            if (FAILED(ex->GetTransformForStream(stream, i, &cat, &t)) || !t) break;
            std::string name = "(no name)";
            bool hw = false, d3d11 = false;
            ComPtr<IMFAttributes> at;
            if (SUCCEEDED(t->GetAttributes(&at)) && at) {
                WCHAR* str = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(at->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &str, &len)) && str) {
                    name = elog::Narrow(str);
                    CoTaskMemFree(str);
                }
                UINT32 len2 = 0;
                hw = SUCCEEDED(at->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &len2));
                d3d11 = MFGetAttributeUINT32(at.Get(), MF_SA_D3D11_AWARE, 0) != 0;
            }
            LOGI("recorder: %s stream transform %lu: %s '%s'%s%s", stream == vStream ? "video" : "audio", (unsigned long)i,
                 GuidCategoryName(cat).c_str(), name.c_str(), hw ? " [hardware]" : "", d3d11 ? " [D3D11-aware]" : "");
        }
    }
}

void Recorder::Impl::Process(const Item& it) {
    encCtx4->Wait(ring.ready.Get(), it.ready);
    const int n = pool->Acquire(3000);
    if (n < 0) {
        ++dropPool;
        if (dropPool.load() == 1) LOGW("recorder: encoder stalled for 3 s (no free NV12 surface) -- dropping frames");
        FreeSlot(it.slot, 0, false);
        return;
    }
    // Exclusive use of the encoder device's immediate context against the MF components that share it.
    ComPtr<ID3D11Device> locked;
    const bool haveLock = devHandle && SUCCEEDED(devMgr->LockDevice(devHandle, IID_PPV_ARGS(&locked), TRUE));
    ID3D11VideoProcessorInputView* view = nullptr;
    if (!srgb) {
        ID3D11ShaderResourceView* srv = linSrv[it.slot].Get();
        ID3D11UnorderedAccessView* uav = linUav.Get();
        encCtx->CSSetShader(linCs.Get(), nullptr, 0);
        encCtx->CSSetShaderResources(0, 1, &srv);
        encCtx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        encCtx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        encCtx->CSSetShaderResources(0, 1, &nullSrv);
        encCtx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        encCtx->CSSetShader(nullptr, nullptr, 0);
        view = linView.Get();
    } else {
        view = inViews[it.slot].Get();
        if (!localCopies.empty()) encCtx->CopyResource(localCopies[it.slot].Get(), ring.tex[it.slot].Get());
    }
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = view;
    HRESULT hr = vctx->VideoProcessorBlt(vp.Get(), outViews[(size_t)n].Get(), 0, 1, &st);
    ++freeCounter;
    encCtx4->Signal(ring.free.Get(), freeCounter);
    encCtx->Flush();
    if (haveLock) devMgr->UnlockDevice(devHandle, FALSE);
    FreeSlot(it.slot, freeCounter, true);
    if (FAILED(hr)) {
        pool->Release(n);
        ++dropPool;
        LOGE("recorder: VideoProcessorBlt failed hr=0x%08lx", (unsigned long)hr);
        return;
    }

    const int64_t rel = it.ts - t0.load();
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMF2DBuffer> b2;
    ComPtr<IMFTrackedSample> tracked;
    ComPtr<IMFSample> sample;
    DWORD len = 0;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12[(size_t)n].Get(), 0, FALSE, &buf);
    if (SUCCEEDED(hr)) hr = buf.As(&b2);
    if (SUCCEEDED(hr)) hr = b2->GetContiguousLength(&len);
    if (SUCCEEDED(hr)) hr = buf->SetCurrentLength(len);
    if (SUCCEEDED(hr)) hr = MFCreateTrackedSample(&tracked);
    if (SUCCEEDED(hr)) hr = tracked.As(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(rel);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(10000000LL / fps);
    if (SUCCEEDED(hr)) hr = tracked->SetAllocator(poolCb[(size_t)n].Get(), nullptr);
    else pool->Release(n);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lk(wm);
        hr = writer ? writer->WriteSample(vStream, sample.Get()) : E_FAIL;
    }
    if (FAILED(hr)) {
        if (++writeErrors <= 5) LOGE("recorder: video WriteSample failed hr=0x%08lx", (unsigned long)hr);
        return;
    }
    ++written;
    lastWrittenTs = rel;
}

void Recorder::Impl::OnAudio(const int16_t* pcm, uint32_t frames, int64_t startQpc) {
    const int64_t t0v = t0.load();
    if (t0v == INT64_MIN) return;               // video has not started yet
    int64_t rel = startQpc - t0v;
    if (rel < 0) {
        const uint64_t skip = (uint64_t)((-rel) * 48000 / 10000000LL) + 1;
        if (skip >= frames) return;
        pcm += skip * 2;
        frames -= (uint32_t)skip;
        rel += (int64_t)(skip * 10000000ull / 48000);
    }
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFSample> sample;
    BYTE* p = nullptr;
    const DWORD bytes = frames * 4;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf)) || FAILED(buf->Lock(&p, nullptr, nullptr))) return;
    memcpy(p, pcm, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    if (FAILED(MFCreateSample(&sample))) return;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(rel);
    sample->SetSampleDuration((LONGLONG)frames * 10000000LL / 48000);
    std::lock_guard<std::mutex> lk(wm);
    if (!writer) return;
    if (SUCCEEDED(writer->WriteSample(aStream, sample.Get()))) audioFrames += frames;
}

void Recorder::Impl::Cleanup() {
    if (hasAudio) audio.Stop();
    {
        std::lock_guard<std::mutex> lk(wm);
        writer.Reset();
    }
    if (devHandle && devMgr) devMgr->CloseDeviceHandle(devHandle);
    devHandle = nullptr;
    inViews.clear();
    localCopies.clear();
    linView.Reset(); linUav.Reset(); linTex.Reset(); linSrv.clear(); linCs.Reset();
    outViews.clear();
    nv12.clear();
    poolCb.clear();
    vp.Reset();
    venum.Reset();
    vctx.Reset();
    vdev.Reset();
    if (ringCreated) {
        // The game thread no longer calls CopyEyes (state != Recording and gameBusy drained).
        const ULONGLONG t = GetTickCount64();
        while (gameBusy.load() > 0 && GetTickCount64() - t < 2000) Sleep(1);
        src->DestroyRing();
        ringCreated = false;
    }
    ring = SharedRing();
    if (encCtx) { encCtx->ClearState(); encCtx->Flush(); }
}
