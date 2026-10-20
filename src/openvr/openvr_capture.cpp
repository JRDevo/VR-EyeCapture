// openvr_capture -- see openvr_capture.h.
#include "openvr_capture.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <shlobj.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture_common.h"
#include "config.h"
#include "control.h"
#include "frame_source.h"
#include "live_share.h"
#include "live_share_proto.h"
#include "log.h"
#include "openvr_hooks.h"
#include "openvr_indicator.h"
#include "openvr_versions.h"
#include "recorder.h"
#include "shared_status.h"

namespace {

// ------------------------------------------------------------------------------------------------ state

struct TexInfo {
    SwapchainInfo info;          // typed capture format, size, array, samples
    DXGI_FORMAT   texFormat = DXGI_FORMAT_UNKNOWN;   // the texture's own format (may be TYPELESS)
    uint64_t      lastUse = 0;
};

struct PendingEye {
    bool       have = false;
    EyeView    v;
    bool       srgb = true;
    bool       hasSubmitPose = false;
    vr::HmdMatrix34_t submitPose{};
    uint64_t   waitSeq = 0;      // the WaitGetPoses call count when this eye arrived
};

// The WaitGetPoses render pose [0] (the HMD): written by WaitGetPoses (any thread) under its OWN small lock -- never g_lock,
// which Submit holds across the capture -- as a 2-deep ring keyed by the call count, so a threaded game (WaitGetPoses on
// one thread, Submit on another) cannot make the capture wait and a pair keeps the pose its first eye saw.
struct WaitPose {
    uint64_t seq = 0;            // WaitGetPoses call number (1-based); 0 = empty
    bool     valid = false;
    vr::HmdMatrix34_t m{};
};

struct OvrState {
    bool         active = false;       // home found and steamvr_enabled
    std::wstring proxyDir, home, gameName, exeName, logPath;
    Config       cfg;
    CaptureEnv   env;
    std::wstring liveShm;
    PFN_VR_GetGenericInterface realGet = nullptr;
    bool         armed = false;        // set at VR_InitInternal (tray running or require_tray = 0)
    bool         inited = false;       // between a successful init and the shutdown
    bool         controlStarted = false;
    std::vector<std::string> unknownLogged;
    // IVRSystem (asked from the real DLL, never hooked)
    void*                sys = nullptr;
    const SystemVersion* sysVer = nullptr;
    bool                 sysTried = false;
    float                displayHz = 0;
    // capture: the graphics API of the first recorded Submit (D3D11: the game's device; D3D12: the game's device + the queue
    // of its D3D12TextureData_t -- the copies run on that queue, like the compositor's own)
    GfxApi                        api = GfxApi::None;
    std::unique_ptr<IFrameSource> src;
    std::unique_ptr<Recorder>     rec;
    std::unique_ptr<LiveShare>    live;
    ComPtr<ID3D11Device>          dev;
    ComPtr<ID3D12Device>          dev12;
    ComPtr<ID3D12CommandQueue>    queue12;
    // the in-headset dot (openvr_indicator.h): created at the first pair of a VR session while vr_indicator = steamvr_indicator = 1.
    // Its 64x64 texture lives on a D3D11 device: the game's (D3D11) or a small private one on the game's adapter (D3D12).
    std::unique_ptr<OvrIndicator> ind;
    IndicatorTracker              indTracker;
    ComPtr<ID3D11Device>          indDev;
    bool                          indDevTried = false;
    bool     srcFailed = false, faulted = false;
    std::map<void*, TexInfo> tex;      // per submitted texture pointer (no destroy event in OpenVR)
    uint64_t texUse = 0;
    PendingEye pend[2];
    // the pose of the pair being assembled: snapshot of the newest WaitGetPoses pose when its FIRST eye arrived
    WaitPose  pairPose;
    bool      loggedPoseRace = false;
    // counters / log-once flags
    uint64_t submits[2] = {0, 0}, pairs = 0, replaced = 0, submitErrors = 0;
    PeriodEstimator period;
    std::string layoutSig;
    int  layoutLogs = 0;
    bool loggedType = false, loggedLens = false, loggedFlipV = false, loggedFlipU = false, loggedArray = false,
         loggedOneEye = false, loggedQi = false, loggedDevice = false, loggedNoSys = false, loggedNoPose = false,
         loggedPoseFrom = false, loggedDepth = false, loggedSubmitErr = false, secondProxy = false, loggedQueue = false,
         loggedNodeMask = false, loggedApiMix = false, loggedNotTex2d = false;
    // autorecord (XR_EYE_CAPTURE_AUTORECORD)
    bool    autoDone = false;
    int64_t autoStopAt = 0;
};

// Heap-allocated and never freed (see layer.cpp: no destructors at DLL unload / process exit).
std::mutex& g_lock = *new std::mutex;
OvrState& g = *new OvrState;
std::mutex& g_ctlLock = *new std::mutex;
std::atomic<bool> g_startRequested{false};
std::atomic<bool> g_active{false};
thread_local int t_depth = 0;   // reentrancy: an inner call (FnTable -> vtable, our own calls) is a plain pass-through
// RAII: the depth comes back down even when the forwarded original faults and unwinds (/EHa) through the hook
struct DepthGuard {
    DepthGuard() { ++t_depth; }
    ~DepthGuard() { --t_depth; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
};
VtablePatchTable& g_patches = *new VtablePatchTable;
std::mutex& g_poseLock = *new std::mutex;   // guards g_wait (never taken together with g_lock held by WaitGetPoses)
WaitPose g_wait[2];
std::atomic<uint64_t> g_waitCalls{0};

// The newest WaitGetPoses pose (any thread).
WaitPose LatestWaitPose() {
    std::lock_guard<std::mutex> lk(g_poseLock);
    return g_wait[0].seq >= g_wait[1].seq ? g_wait[0] : g_wait[1];
}

// FnTable copies (one per known version), handed to the game instead of the real table.
void* g_fnCopy[kCompositorVersionCount][kFnTableMax];
void* g_fnOrigWait[kCompositorVersionCount];
void* g_fnOrigSubmit[kCompositorVersionCount];
void* g_fnRealTable[kCompositorVersionCount];

std::string Narrow(const std::wstring& w) { return elog::Narrow(w); }

// "SteamVR D3D11" / "SteamVR D3D12": the API shown in the tray / status and the record start line.
const char* ApiLabel() { return g.api == GfxApi::D3D12 ? "SteamVR D3D12" : "SteamVR D3D11"; }
ID3D11Device* IndicatorDevice();   // below, with the frame sources

// ------------------------------------------------------------------------------------------------ control hooks

CaptureSnapshot Snapshot() {
    CaptureSnapshot s;
    std::lock_guard<std::mutex> lk(g_lock);
    s.game = g.exeName;
    s.outputDir = g.cfg.outputDir;
    if (g.src && g.rec) {
        s.capable = true;
        s.rec = g.rec->Status();
        if (g_startRequested.load() && s.rec.state == RecState::Failed) s.rec.state = RecState::Idle;   // a new start is pending
        s.api = g.api == GfxApi::D3D12 ? L"SteamVR D3D12" : L"SteamVR D3D11";
        s.runtime = L"SteamVR";
    }
    return s;
}

void RequestStart() { g_startRequested = true; }

void RequestStop() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_startRequested = false;
    if (g.rec) g.rec->Stop();
}

// ------------------------------------------------------------------------------------------------ IVRSystem

// Under g_lock. Asks the real DLL for the newest IVRSystem we know (never through our own export).
void EnsureSystem() {
    if (g.sysTried || !g.realGet) return;
    g.sysTried = true;
    std::string tried;
    for (int i = 0; i < kSystemVersionCount; ++i) {
        vr::EVRInitError e = vr::VRInitError_None;
        void* p = nullptr;
        {
            DepthGuard dg;
            p = g.realGet(kSystemVersions[i].name, &e);
        }
        if (p && e == vr::VRInitError_None) {
            g.sys = p;
            g.sysVer = &kSystemVersions[i];
            float hz = 0;
            int err = 0;
            if (SysGetDisplayHz(p, kSystemVersions[i], &hz, &err)) g.displayHz = hz;
            float fl[2][4] = {};
            for (int eye = 0; eye < 2; ++eye)
                SysGetProjectionRaw(p, kSystemVersions[i], (vr::EVREye)eye, &fl[eye][0], &fl[eye][1], &fl[eye][2], &fl[eye][3]);
            LOGI("steamvr: %s (slots: GetProjectionRaw %u, GetEyeToHeadTransform %u, float property %u) | display %.2f Hz%s | "
                 "projection raw L (l %.3f r %.3f t %.3f b %.3f) R (l %.3f r %.3f t %.3f b %.3f)",
                 kSystemVersions[i].name, kSystemVersions[i].slotProjectionRaw, kSystemVersions[i].slotEyeToHead,
                 kSystemVersions[i].slotFloatProperty, g.displayHz, g.displayHz > 0 ? "" : " (not reported)",
                 fl[0][0], fl[0][1], fl[0][2], fl[0][3], fl[1][0], fl[1][1], fl[1][2], fl[1][3]);
            return;
        }
        if (!tried.empty()) tried += ", ";
        tried += std::string(kSystemVersions[i].name) + " (" + std::to_string((int)e) + ")";
    }
    LOGW("steamvr: no IVRSystem interface (%s) -- recordings have no head pose / fov (raw eyes; head smoothing, "
         "symmetric_fov and the dome need them)", tried.c_str());
}

// ------------------------------------------------------------------------------------------------ capture

const char* LayoutName(const EyeView& L, const EyeView& R) {
    if (L.swapchainKey != R.swapchainKey) return "TWO_TEXTURES";
    if (L.arrayIndex != R.arrayIndex) return "TEXTURE_ARRAY (one texture, slice 0/1)";
    if (L.x != R.x || L.y != R.y) return "ONE_TEXTURE_TWO_RECTS (wide texture, bounds)";
    return "SAME_TEXTURE_SAME_RECT (both eyes identical -- recorded twice)";
}

void LogLayout(const EyeView& L, const EyeView& R, bool srgb, const char* poseFrom) {
    char sig[512];
    snprintf(sig, sizeof(sig), "%s | L %ux%u+%d+%d slice %u | R %ux%u+%d+%d slice %u", LayoutName(L, R), L.w, L.h, L.x, L.y,
             L.arrayIndex, R.w, R.h, R.x, R.y, R.arrayIndex);
    if (g.layoutSig == sig) return;
    g.layoutSig = sig;
    if (++g.layoutLogs > 20) return;
    LOGI("frame layout (pair %llu): %s", (unsigned long long)g.pairs, sig);
    LOGI("  left : %s %ux%u array %u samples %u | right: %s %ux%u array %u samples %u | %s | pose: %s",
         DxgiFormatName((DXGI_FORMAT)L.sc->format), L.sc->width, L.sc->height, L.sc->arraySize, L.sc->sampleCount,
         DxgiFormatName((DXGI_FORMAT)R.sc->format), R.sc->width, R.sc->height, R.sc->arraySize, R.sc->sampleCount,
         srgb ? "sRGB-encoded" : "linear", poseFrom);
}

// Under g_lock: the eye's head pose (per steamvr_pose_source) + eye-to-head + projection -> EyeView pose / fov.
void FillPose(EyeView& e, int eye, const PendingEye& p, const char** from) {
    e.hasPose = false;
    const vr::HmdMatrix34_t* head = nullptr;
    const SteamVrPoseSource ps = g.cfg.steamvr.poseSource;
    if (p.hasSubmitPose && ps != SteamVrPoseSource::WaitGetPoses) { head = &p.submitPose; *from = "Submit (TextureWithPose)"; }
    else if (g.pairPose.valid && ps != SteamVrPoseSource::Submit) { head = &g.pairPose.m; *from = "WaitGetPoses render pose"; }
    else *from = "none (raw eyes)";
    if (!head) return;
    EnsureSystem();
    if (!g.sys) { *from = "none (no IVRSystem)"; return; }
    const vr::HmdMatrix34_t e2h = SysGetEyeToHead(g.sys, *g.sysVer, (vr::EVREye)eye);
    const vr::HmdMatrix34_t m = ComposeMatrix(*head, e2h);
    float l = 0, r = 0, t = 0, b = 0;
    SysGetProjectionRaw(g.sys, *g.sysVer, (vr::EVREye)eye, &l, &r, &t, &b);
    if (!MatrixToPose(m, e.orientation, e.position)) { *from = "none (pose not a rotation)"; return; }
    FovFromProjectionRaw(l, r, t, b, e.fov);
    e.hasPose = true;
}

void StartRecording(const EyeView& L, const EyeView& R, bool srgb) {
    const DXGI_FORMAT stage = StageFormatForDxgi((DXGI_FORMAT)L.sc->format);
    const DXGI_FORMAT stageR = StageFormatForDxgi((DXGI_FORMAT)R.sc->format);
    if (stage == DXGI_FORMAT_UNKNOWN || stageR == DXGI_FORMAT_UNKNOWN) {
        LOGE("record: texture format %s / %s is not supported for capture", DxgiFormatName((DXGI_FORMAT)L.sc->format),
             DxgiFormatName((DXGI_FORMAT)R.sc->format));
        return;
    }
    if (stage != stageR) {
        LOGE("record: left and right eye textures have different formats (%s / %s) -- not supported",
             DxgiFormatName((DXGI_FORMAT)L.sc->format), DxgiFormatName((DXGI_FORMAT)R.sc->format));
        return;
    }
    if (g.cfg.gamma == GammaMode::Srgb) srgb = true;
    else if (g.cfg.gamma == GammaMode::Linear) srgb = false;
    const char* fpsFrom = "config";
    const unsigned fps = PickFps(g.cfg, g.period.periodNs, g.period.samples, g.displayHz, "Prop_DisplayFrequency_Float", &fpsFrom);
    const std::wstring name = RecordingFileName(g.exeName, g.cfg.dome.projection);
    SHCreateDirectoryExW(nullptr, g.cfg.outputDir.c_str(), nullptr);
    RecorderSettings rs;
    rs.path = g.cfg.outputDir + L"\\" + name;
    FillRecorderSettings(g.cfg, L, R, fps, rs);
    const UINT W = L.w + R.w, H = L.h > R.h ? L.h : R.h;
    LOGI("record: start %s | %s | %ux%u SBS (eyes %ux%u + %ux%u) | %u fps from %s (display %.1f Hz, measured %.2f) | %s",
         Narrow(rs.path).c_str(), ApiLabel(), W, H, L.w, L.h, R.w, R.h, fps, fpsFrom, g.displayHz,
         g.period.periodNs > 0 ? 1e9 / g.period.periodNs : 0.0, srgb ? "sRGB-encoded" : "linear");
    if (!g.rec->Start(g.src.get(), rs, W, H, L.w, stage, srgb))
        LOGW("record: previous recording still finalizing -- start ignored");
}

// Under g_lock, inside the second eye's Submit, before it is forwarded.
void CapturePair() {
    ++g.pairs;
    g.period.Add(QpcNow100ns() * 100);
    bool wantStart = g_startRequested.exchange(false);
    if (g.env.autoRecordSec > 0 && !g.autoDone) {
        g.autoDone = true;
        wantStart = true;
        g.autoStopAt = QpcNow100ns() + (int64_t)(g.env.autoRecordSec * 1e7);
    }
    if (g.autoStopAt && QpcNow100ns() >= g.autoStopAt) {
        g.autoStopAt = 0;
        LOGI("record: autorecord time reached -- stopping");
        g.rec->Stop();
    }
    EnsureSystem();   // once: display refresh (fps) + the eye transforms / projection for the poses
    EyeView eyes[2];
    const char* poseFrom = "none";
    for (int e = 0; e < 2; ++e) {
        eyes[e] = g.pend[e].v;
        FillPose(eyes[e], e, g.pend[e], &poseFrom);
    }
    const bool srgb = g.pend[0].srgb;
    if (!g.loggedPoseFrom || (!eyes[0].hasPose && !g.loggedNoPose)) {
        if (!eyes[0].hasPose) g.loggedNoPose = true;
        g.loggedPoseFrom = true;
        LOGI("steamvr: head pose of the recorded frames: %s (steamvr_pose_source = %s)", poseFrom,
             SteamVrPoseSourceKey(g.cfg.steamvr.poseSource));
    }
    {
        // a recording is about to start: pick up config.ini edits (Settings window) before swap_eyes / dome are used
        const RecState rs0 = g.rec->State();
        if (wantStart && rs0 != RecState::Starting && rs0 != RecState::Recording) ReloadConfigForRecording(g.home, g.cfg);
    }
    if (g.cfg.swapEyes) std::swap(eyes[0], eyes[1]);   // swap_eyes = 1: right eye goes into the left half
    LogLayout(eyes[0], eyes[1], srgb, poseFrom);
    const RecState rs = g.rec->State();
    if (wantStart && rs != RecState::Starting && rs != RecState::Recording) StartRecording(eyes[0], eyes[1], srgb);
    const int64_t ts = QpcNow100ns();
    if (g.rec->State() == RecState::Recording) g.rec->SubmitFrame(ts, eyes[0], eyes[1], 0, nullptr);
    if (g.live) {
        DXGI_FORMAT stage = StageFormatForDxgi((DXGI_FORMAT)eyes[0].sc->format);
        if (stage != StageFormatForDxgi((DXGI_FORMAT)eyes[1].sc->format)) stage = DXGI_FORMAT_UNKNOWN;   // not shareable
        bool s = srgb;
        if (g.cfg.gamma == GammaMode::Srgb) s = true;
        else if (g.cfg.gamma == GammaMode::Linear) s = false;
        g.live->Frame(eyes[0], eyes[1], stage, s, ts, g.cfg.head, false, nullptr);
    }
    // the dot: an overlay the compositor draws after this Submit -- the eye images above are already copied
    if (g.cfg.indicator.enabled && g.cfg.steamvr.indicator) {
        if (!g.ind) g.ind = std::make_unique<OvrIndicator>(g.cfg.indicator);
        const RecState st = g.rec->State();
        std::string msg;
        if (g.indTracker.NeedsMessage(st)) msg = g.rec->Status().message;
        const double now = (double)QpcNow100ns() * 1e-7;
        const bool live = g.cfg.indicator.live && g.live && g.live->Active();
        g.ind->Frame(g.realGet, IndicatorDevice(), g.indTracker.Update(st, msg, now, live), now);
    }
}

// Under g_lock: the recorder (+ live share) once a frame source exists.
void MakeRecorder(const LiveShareParams& lp) {
    g.rec = std::make_unique<Recorder>();
    if (g.cfg.liveShare) g.live = std::make_unique<LiveShare>(lp);
}

// Under g_lock: one graphics API per run -- the first one that was recorded (a game never mixes them; logged once if it does).
bool SameApi(GfxApi a) {
    if (g.api == GfxApi::None || g.api == a) return true;
    if (!g.loggedApiMix) {
        g.loggedApiMix = true;
        LOGW("steamvr: a %s texture was submitted after %s ones -- only the first API is recorded", GfxApiName(a), GfxApiName(g.api));
    }
    return false;
}

// Under g_lock: the frame source / recorder on the game's device at the first armed D3D11 Submit.
bool EnsureSource11(ID3D11Texture2D* t) {
    if (!SameApi(GfxApi::D3D11)) return false;
    ComPtr<ID3D11Device> dev;
    t->GetDevice(&dev);
    if (g.src) {
        if (dev.Get() != g.dev.Get() && !g.loggedDevice) {
            g.loggedDevice = true;
            LOGW("steamvr: a texture from another D3D11 device was submitted -- only the first device is recorded");
        }
        return dev.Get() == g.dev.Get();
    }
    if (g.srcFailed) return false;
    g.src = CreateD3D11FrameSource(dev.Get());
    if (!g.src) {
        g.srcFailed = true;
        LOGE("steamvr: no D3D11 frame source on the game's device -- capture disabled for this run");
        return false;
    }
    g.api = GfxApi::D3D11;
    g.dev = dev;
    LiveShareParams lp;
    lp.api = GfxApi::D3D11;
    lp.dev11 = dev.Get();
    lp.game = g.exeName;
    lp.shmName = g.liveShm;
    MakeRecorder(lp);
    LOGI("steamvr: capture AVAILABLE on the game's D3D11 device (first Submit)");
    return true;
}

// Under g_lock: the frame source / recorder at the first armed D3D12 Submit -- on the resource's device and on the queue the
// game hands to the compositor (D3D12TextureData_t::m_pCommandQueue): the copies are recorded into our own command list and
// executed on that queue after the game's rendering, exactly where the compositor reads the texture; no CPU wait.
bool EnsureSource12(ID3D12Resource* res, ID3D12CommandQueue* queue) {
    if (!SameApi(GfxApi::D3D12)) return false;
    ComPtr<ID3D12Device> dev;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
        if (!g.loggedQi) { g.loggedQi = true; LOGW("steamvr: D3D12 resource %p has no device -- not recorded", (void*)res); }
        return false;
    }
    if (g.src) {
        if (dev.Get() != g.dev12.Get()) {
            if (!g.loggedDevice) {
                g.loggedDevice = true;
                LOGW("steamvr: a texture from another D3D12 device was submitted -- only the first device is recorded");
            }
            return false;
        }
        if (queue != g.queue12.Get()) {
            if (!g.loggedQueue) {
                g.loggedQueue = true;
                LOGW("steamvr: a D3D12 Submit names another command queue (%p, recording on %p) -- those eyes are not recorded "
                     "(a copy on a queue the texture was not rendered on would not be ordered after the rendering)",
                     (void*)queue, (void*)g.queue12.Get());
            }
            return false;
        }
        return true;
    }
    if (g.srcFailed) return false;
    const D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // a compute / copy queue cannot take the eye texture out of PIXEL_SHADER_RESOURCE (nor resolve MSAA)
        g.srcFailed = true;
        LOGE("steamvr: the game submits its D3D12 eye textures with a %s queue (type %d), not a direct queue -- capture disabled "
             "for this run", qd.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute" : "copy/other", (int)qd.Type);
        return false;
    }
    g.src = CreateD3D12FrameSource(dev.Get(), queue);
    if (!g.src) {
        g.srcFailed = true;
        LOGE("steamvr: no D3D12 frame source on the game's device -- capture disabled for this run");
        return false;
    }
    g.api = GfxApi::D3D12;
    g.dev12 = dev;
    g.queue12 = queue;
    LiveShareParams lp;
    lp.api = GfxApi::D3D12;
    lp.dev12 = dev.Get();
    lp.queue12 = queue;
    lp.game = g.exeName;
    lp.shmName = g.liveShm;
    MakeRecorder(lp);
    LOGI("steamvr: capture AVAILABLE on the game's D3D12 device, copies on its direct queue %p (first Submit; eye textures "
         "taken as PIXEL_SHADER_RESOURCE and left in it)", (void*)queue);
    return true;
}

// Under g_lock: the D3D11 device the dot's texture lives on -- the game's own (D3D11), or a small private device on the D3D12
// game's adapter (an IVROverlay texture is independent of the scene application's API; nothing runs on the game's queue).
ID3D11Device* IndicatorDevice() {
    if (g.api == GfxApi::D3D11) return g.dev.Get();
    if (g.api != GfxApi::D3D12 || !g.dev12) return nullptr;
    if (g.indDevTried) return g.indDev.Get();
    g.indDevTried = true;
    const LUID luid = g.dev12->GetAdapterLuid();
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter1> ad;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad)))) {
        LOGW("indicator: the D3D12 game's adapter was not found -- no dot");
        return nullptr;
    }
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl,
                                         (UINT)(sizeof(fl) / sizeof(fl[0])), D3D11_SDK_VERSION, &g.indDev, nullptr, nullptr);
    if (FAILED(hr)) {
        LOGW("indicator: D3D11CreateDevice on the D3D12 game's adapter failed hr=0x%08lx -- no dot", (unsigned long)hr);
        g.indDev.Reset();
        return nullptr;
    }
    LOGI("indicator: D3D12 game -- the dot's texture lives on a small private D3D11 device on the same adapter");
    return g.indDev.Get();
}

// What LookupTexture needs of a submitted texture (D3D11_TEXTURE2D_DESC / D3D12_RESOURCE_DESC).
struct TexDesc {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0, arraySize = 1, mips = 1, samples = 1;
    int32_t d3d12State = -1;   // D3D12: the hand-off state (SwapchainInfo::d3d12State)
    char extra[96] = "";       // API-specific part of the log line
};

// Under g_lock. The texture's cached description (ForgetSwapchain when a pointer we saw comes back different).
TexInfo* LookupTexture(void* t, const TexDesc& d, bool srgb) {
    const DXGI_FORMAT typed = TypedCaptureFormat(d.format, srgb);
    auto it = g.tex.find(t);
    if (it != g.tex.end()) {
        TexInfo& ti = it->second;
        if (ti.texFormat == d.format && ti.info.format == (int64_t)typed && ti.info.width == d.width &&
            ti.info.height == d.height && ti.info.arraySize == d.arraySize && ti.info.sampleCount == d.samples &&
            ti.info.mipCount == d.mips && ti.info.d3d12State == d.d3d12State) {
            ti.lastUse = ++g.texUse;
            return &ti;
        }
        LOGI("steamvr: texture %p changed (now %ux%u %s array %u samples %u) -- its caches are dropped", t, d.width, d.height,
             DxgiFormatName(d.format), d.arraySize, d.samples);
        if (g.src) g.src->ForgetSwapchain(t);
        for (PendingEye& p : g.pend)   // a pending eye must not keep pointing at the erased description
            if (p.have && p.v.swapchainKey == (const void*)t) p.have = false;
        g.tex.erase(it);
    }
    if (g.tex.size() >= 32) {   // forget the least recently used one (never one a pending eye still points at)
        auto victim = g.tex.end();
        for (auto i = g.tex.begin(); i != g.tex.end(); ++i) {
            if ((g.pend[0].have && g.pend[0].v.swapchainKey == i->first) || (g.pend[1].have && g.pend[1].v.swapchainKey == i->first)) continue;
            if (victim == g.tex.end() || i->second.lastUse < victim->second.lastUse) victim = i;
        }
        if (victim != g.tex.end()) {
            if (g.src) g.src->ForgetSwapchain(victim->first);
            g.tex.erase(victim);
        }
    }
    TexInfo& ti = g.tex[t];
    ti.texFormat = d.format;
    ti.info.format = (int64_t)typed;
    ti.info.width = d.width;
    ti.info.height = d.height;
    ti.info.arraySize = d.arraySize ? d.arraySize : 1;
    ti.info.mipCount = d.mips ? d.mips : 1;
    ti.info.sampleCount = d.samples ? d.samples : 1;
    ti.info.faceCount = 1;
    ti.info.d3d12State = d.d3d12State;
    ti.lastUse = ++g.texUse;
    LOGI("steamvr: texture %p: %ux%u %s (captured as %s) array %u mips %u samples %u %s", t, d.width, d.height,
         DxgiFormatName(d.format), DxgiFormatName(typed), d.arraySize, d.mips, d.samples, d.extra);
    return &ti;
}

// Under g_lock: the submitted texture -> the image to copy + its description; false = not recorded (logged once).
bool ResolveSubmittedTexture(const vr::Texture_t* tex, void** image, TexDesc* d) {
    if (tex->eType == vr::TextureType_DirectX) {
        IUnknown* unk = static_cast<IUnknown*>(tex->handle);
        ComPtr<ID3D11Texture2D> t2d;
        if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&t2d))) || !t2d) {
            if (!g.loggedQi) { g.loggedQi = true; LOGW("steamvr: Submit handle %p is not an ID3D11Texture2D -- not recorded", tex->handle); }
            return false;
        }
        if (!EnsureSource11(t2d.Get())) return false;
        D3D11_TEXTURE2D_DESC td{};
        t2d->GetDesc(&td);
        d->format = td.Format;
        d->width = td.Width;
        d->height = td.Height;
        d->arraySize = td.ArraySize;
        d->mips = td.MipLevels;
        d->samples = td.SampleDesc.Count;
        snprintf(d->extra, sizeof d->extra, "D3D11 bind 0x%x misc 0x%x%s", td.BindFlags, td.MiscFlags,
                 (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) ? " (KEYED MUTEX: watch for stale frames)" : "");
        *image = tex->handle;   // the ID3D11Texture2D itself
        return true;
    }
    // TextureType_DirectX12: handle = the game's D3D12TextureData_t (often on its stack: read now, never kept)
    const vr::D3D12TextureData_t* td = static_cast<const vr::D3D12TextureData_t*>(tex->handle);
    if (!td->m_pResource || !td->m_pCommandQueue) {
        if (!g.loggedQi) {
            g.loggedQi = true;
            LOGW("steamvr: D3D12 Submit with resource %p / queue %p -- not recorded", (void*)td->m_pResource, (void*)td->m_pCommandQueue);
        }
        return false;
    }
    ComPtr<ID3D12Resource> res;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(reinterpret_cast<IUnknown*>(td->m_pResource)->QueryInterface(IID_PPV_ARGS(&res))) || !res ||
        FAILED(reinterpret_cast<IUnknown*>(td->m_pCommandQueue)->QueryInterface(IID_PPV_ARGS(&q))) || !q) {
        if (!g.loggedQi) {
            g.loggedQi = true;
            LOGW("steamvr: D3D12 Submit: %p / %p are not an ID3D12Resource / ID3D12CommandQueue -- not recorded",
                 (void*)td->m_pResource, (void*)td->m_pCommandQueue);
        }
        return false;
    }
    if (td->m_nNodeMask > 1 && !g.loggedNodeMask) {
        g.loggedNodeMask = true;
        LOGW("steamvr: D3D12 Submit with node mask 0x%x (multi-GPU linked adapter) -- recorded on the queue's node", td->m_nNodeMask);
    }
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        if (!g.loggedNotTex2d) {
            g.loggedNotTex2d = true;
            LOGW("steamvr: D3D12 Submit of a resource with dimension %d (not a 2D texture) -- not recorded", (int)rd.Dimension);
        }
        return false;
    }
    if (!EnsureSource12(res.Get(), q.Get())) return false;
    d->format = rd.Format;
    d->width = (uint32_t)rd.Width;
    d->height = rd.Height;
    d->arraySize = rd.DepthOrArraySize;
    d->mips = rd.MipLevels;
    d->samples = rd.SampleDesc.Count;
    d->d3d12State = (int32_t)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    snprintf(d->extra, sizeof d->extra, "D3D12 flags 0x%x queue %p", (unsigned)rd.Flags, (void*)td->m_pCommandQueue);
    *image = td->m_pResource;   // the resource, never the (temporary) D3D12TextureData_t
    return true;
}

// Under g_lock, before the game's Submit is forwarded.
void OnSubmitImpl(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) {
    const int e = eye == vr::Eye_Right ? 1 : 0;
    ++g.submits[e];
    if (!tex || !tex->handle) return;
    if (tex->eType != vr::TextureType_DirectX && tex->eType != vr::TextureType_DirectX12) {
        if (!g.loggedType) {
            g.loggedType = true;
            LOGW("steamvr: textures of type %s (%d) are not supported yet (D3D11 / D3D12 only) -- forwarded, NOT recorded",
                 TextureTypeName(tex->eType), (int)tex->eType);
        }
        return;
    }
    if ((flags & vr::Submit_LensDistortionAlreadyApplied) && !g.loggedLens) {
        g.loggedLens = true;
        LOGW("steamvr: Submit_LensDistortionAlreadyApplied -- the game submits pre-distorted eyes; recorded as they are");
    }
    if ((flags & vr::Submit_TextureWithDepth) && !g.loggedDepth) {
        g.loggedDepth = true;
        LOGI("steamvr: Submit_TextureWithDepth -- depth is ignored (colour recorded)");
    }
    void* image = nullptr;
    TexDesc td;
    if (!ResolveSubmittedTexture(tex, &image, &td)) return;
    const bool srgbData = SubmitIsSrgb(tex->eColorSpace, td.format);
    TexInfo* ti = LookupTexture(image, td, srgbData);
    if (!ti) return;
    PendingEye& p = g.pend[e];
    if (p.have) ++g.replaced;   // a second Left (or Right) before the other eye: the newer one wins
    p = PendingEye();
    EyeView& v = p.v;
    v.image = image;            // never AddRef'd: copied before this Submit returns (or before the other eye's)
    v.sc = &ti->info;
    v.swapchainKey = image;
    v.arrayIndex = 0;
    if (ti->info.arraySize >= 2) {
        v.arrayIndex = (uint32_t)e;
        if (!g.loggedArray) {
            g.loggedArray = true;
            LOGI("steamvr: texture array (%u slices): recording slice %d for the left eye, %d for the right", ti->info.arraySize, 0, 1);
        }
    }
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    bool fu = false, fv = false;
    if (!BoundsToRect(bounds, ti->info.width, ti->info.height, &x, &y, &w, &h, &fu, &fv)) {
        p.have = false;
        return;
    }
    if (fv && !g.loggedFlipV) {
        g.loggedFlipV = true;
        LOGW("steamvr: bounds vMin > vMax (a flipped texture) -- recorded upside down in this version");
    }
    if (fu && !g.loggedFlipU) {
        g.loggedFlipU = true;
        LOGW("steamvr: bounds uMin > uMax (a mirrored texture) -- recorded mirrored in this version");
    }
    v.x = x; v.y = y; v.w = w; v.h = h;
    if (flags & vr::Submit_TextureWithPose) {
        p.hasSubmitPose = true;
        p.submitPose = static_cast<const vr::VRTextureWithPose_t*>(tex)->mDeviceToAbsoluteTracking;
    }
    p.srgb = srgbData;
    p.waitSeq = g_waitCalls.load();
    if (!g.pend[1 - e].have) {
        g.pairPose = LatestWaitPose();   // the first eye of this pair: its pose is the pair's
    } else if (p.waitSeq != g.pend[1 - e].waitSeq && !g.loggedPoseRace) {
        g.loggedPoseRace = true;
        LOGI("steamvr: WaitGetPoses ran between the two eyes of a pair (call %llu -> %llu; a threaded game) -- the pose the first eye "
             "saw is used for both (logged once)", (unsigned long long)g.pend[1 - e].waitSeq, (unsigned long long)p.waitSeq);
    }
    p.have = true;
    if (g.pend[0].have && g.pend[1].have) {
        CapturePair();
        g.pend[0].have = g.pend[1].have = false;
    } else if (!g.loggedOneEye && g.submits[e] >= 300 && g.pairs == 0) {
        g.loggedOneEye = true;
        LOGW("steamvr: 300 Submits for the %s eye and never one for the other -- a one-eye game is not recorded",
             e ? "right" : "left");
    }
}

// Hardware faults (access violation ...) are caught here; a C++ exception (0xE06D7363) continues the search so the
// unwinding runs its destructors and the catch (...) in OnSubmit gets it.
int SubmitFaultFilter(unsigned long code) { return code == 0xE06D7363ul ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER; }
bool OnSubmitSafe(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) {
    __try {
        OnSubmitImpl(eye, tex, bounds, flags);
        return true;
    } __except (SubmitFaultFilter(GetExceptionCode())) {
        return false;
    }
}

// Any thread (the game's render thread), before the Submit is forwarded.
void OnSubmit(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g.active || !g.armed || !g.inited || g.faulted) return;
    bool ok = false;
    try { ok = OnSubmitSafe(eye, tex, bounds, flags); } catch (...) { ok = false; }
    if (!ok) {
        g.faulted = true;
        g.pend[0].have = g.pend[1].have = false;
        LOGE("capture faulted inside Submit -- capture disabled for this run, the game continues");
        if (g.rec) g.rec->Stop();
    }
}

void OnSubmitResult(vr::EVREye eye, vr::EVRCompositorError r) {
    if (r == vr::VRCompositorError_None) return;
    std::lock_guard<std::mutex> lk(g_lock);
    ++g.submitErrors;
    if (!g.loggedSubmitErr && g.active && g.armed) {
        g.loggedSubmitErr = true;
        LOGW("steamvr: the runtime's Submit(%s) returned error %d (passed to the game unchanged; logged once)",
             eye == vr::Eye_Right ? "right" : "left", (int)r);
    }
}

// Any thread. Never takes g_lock (a Submit on another thread may hold it across the capture).
void OnWaitGetPoses(const vr::TrackedDevicePose_t* render, uint32_t n, vr::EVRCompositorError r) {
    WaitPose w;
    w.valid = r == vr::VRCompositorError_None && render && n >= 1 && render[0].bPoseIsValid;
    if (w.valid) w.m = render[0].mDeviceToAbsoluteTracking;
    std::lock_guard<std::mutex> lk(g_poseLock);
    w.seq = g_waitCalls.load() + 1;
    if (!w.valid) {   // keep the last valid pose but count the call
        const WaitPose& prev = g_wait[(w.seq + 1) % 2];
        if (prev.valid) { w.valid = true; w.m = prev.m; }
    }
    g_wait[w.seq % 2] = w;
    g_waitCalls.store(w.seq);
}

// ------------------------------------------------------------------------------------------------ hooks (C++ vtables)

typedef vr::EVRCompositorError (*PFN_CppWait)(void*, vr::TrackedDevicePose_t*, uint32_t, vr::TrackedDevicePose_t*, uint32_t);
typedef vr::EVRCompositorError (*PFN_CppSubmit)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

// The record of this object's vtable (found even after the hooks were taken out: a call already inside a hook forwards).
const VtablePatch* FindPatch(void* self) { return g_patches.Find(*(void***)self); }

vr::EVRCompositorError HookCpp_WaitGetPoses(void* self, vr::TrackedDevicePose_t* render, uint32_t nRender,
                                            vr::TrackedDevicePose_t* game, uint32_t nGame) {
    const VtablePatch* pr = FindPatch(self);
    const PFN_CppWait orig = pr ? (PFN_CppWait)pr->origWait : nullptr;
    if (!orig) return vr::VRCompositorError_RequestFailed;
    if (t_depth > 0 || pr->restored) return orig(self, render, nRender, game, nGame);   // restored: a plain pass-through
    vr::EVRCompositorError r;
    {
        DepthGuard dg;
        r = orig(self, render, nRender, game, nGame);
    }
    OnWaitGetPoses(render, nRender, r);
    return r;
}

vr::EVRCompositorError HookCpp_Submit(void* self, vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* b,
                                      vr::EVRSubmitFlags flags) {
    const VtablePatch* pr = FindPatch(self);
    const PFN_CppSubmit orig = pr ? (PFN_CppSubmit)pr->origSubmit : nullptr;
    if (!orig) return vr::VRCompositorError_RequestFailed;
    if (t_depth > 0 || pr->restored) return orig(self, eye, tex, b, flags);
    vr::EVRCompositorError r;
    {
        DepthGuard dg;
        OnSubmit(eye, tex, b, flags);
        r = orig(self, eye, tex, b, flags);
    }
    OnSubmitResult(eye, r);
    return r;
}

// ------------------------------------------------------------------------------------------------ hooks (FnTable copies)

template <int V>
vr::EVRCompositorError FnHook_WaitGetPoses(vr::TrackedDevicePose_t* render, uint32_t nRender, vr::TrackedDevicePose_t* game,
                                           uint32_t nGame) {
    const vr::PFN_FnWaitGetPoses orig = (vr::PFN_FnWaitGetPoses)g_fnOrigWait[V];
    if (!orig) return vr::VRCompositorError_RequestFailed;
    if (t_depth > 0) return orig(render, nRender, game, nGame);
    vr::EVRCompositorError r;
    {
        DepthGuard dg;
        r = orig(render, nRender, game, nGame);
    }
    OnWaitGetPoses(render, nRender, r);
    return r;
}

template <int V>
vr::EVRCompositorError FnHook_Submit(vr::EVREye eye, vr::Texture_t* tex, vr::VRTextureBounds_t* b, vr::EVRSubmitFlags flags) {
    const vr::PFN_FnSubmit orig = (vr::PFN_FnSubmit)g_fnOrigSubmit[V];
    if (!orig) return vr::VRCompositorError_RequestFailed;
    if (t_depth > 0) return orig(eye, tex, b, flags);
    vr::EVRCompositorError r;
    {
        DepthGuard dg;
        OnSubmit(eye, tex, b, flags);
        r = orig(eye, tex, b, flags);
    }
    OnSubmitResult(eye, r);
    return r;
}

template <int V> struct FnHooks {
    static void Fill(void** wait, void** submit) {
        *wait = (void*)&FnHook_WaitGetPoses<V>;
        *submit = (void*)&FnHook_Submit<V>;
    }
};

void FnHooksFor(int v, void** wait, void** submit) {
    switch (v) {
    case 0: FnHooks<0>::Fill(wait, submit); break;
    case 1: FnHooks<1>::Fill(wait, submit); break;
    case 2: FnHooks<2>::Fill(wait, submit); break;
    case 3: FnHooks<3>::Fill(wait, submit); break;
    case 4: FnHooks<4>::Fill(wait, submit); break;
    case 5: FnHooks<5>::Fill(wait, submit); break;
    case 6: FnHooks<6>::Fill(wait, submit); break;
    case 7: FnHooks<7>::Fill(wait, submit); break;
    case 8: FnHooks<8>::Fill(wait, submit); break;
    case 9: FnHooks<9>::Fill(wait, submit); break;
    case 10: FnHooks<10>::Fill(wait, submit); break;
    default: *wait = *submit = nullptr; break;
    }
}
static_assert(kCompositorVersionCount == 11, "FnHooksFor covers 11 compositor versions");

// ------------------------------------------------------------------------------------------------ control thread

// One proxy per process records (control.cpp is a process singleton): the first one that inits claims the process with a
// named object; a second copy (a game with two openvr_api.dll folders loaded, e.g. a launcher + the game) stays a pure
// forwarder and logs which module has the claim. Under g_lock. The named object is also the marker EyeCaptureMirror (quick
// mode) looks for: a process this proxy records is left to it (EYECAP_PROXY_MARKER_PREFIX, src/shared_status.h).
bool ClaimProcess() {
    static bool claimed = false;
    if (claimed) return true;
    if (g.secondProxy) return false;
    const std::wstring name = EYECAP_PROXY_MARKER_PREFIX + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateMutexW(nullptr, FALSE, name.c_str());   // kept open (leaked) while the process lives
    if (h && GetLastError() != ERROR_ALREADY_EXISTS) {
        claimed = true;
        return true;
    }
    if (h) CloseHandle(h);
    g.secondProxy = true;
    g.active = false;
    g_active = false;
    std::string other = "?";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&ClaimProcess, &self);
    HMODULE mods[512];
    DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; ++i) {
            if (mods[i] == self || !GetProcAddress(mods[i], EYECAP_PROXY_VERSION_EXPORT)) continue;
            wchar_t p[MAX_PATH] = L"";
            GetModuleFileNameW(mods[i], p, MAX_PATH);
            other = Narrow(p);
            break;
        }
    }
    LOGW("steamvr: another VR EyeCapture proxy already records this process (%s) -- this copy stays a pure forwarder", other.c_str());
    return false;
}

void EnsureControl() {
    if (!g.env.trayIpc) return;
    std::lock_guard<std::mutex> lk(g_ctlLock);
    ControlStart(g.cfg, g.logPath, g.cfg.loadedFrom.empty() ? ConfigPath(g.home) : g.cfg.loadedFrom,
                 ControlHooks{Snapshot, RequestStart, RequestStop});   // no-op if already running
    g.controlStarted = true;
}

void StopControl() {
    std::lock_guard<std::mutex> lk(g_ctlLock);
    if (!g.controlStarted) return;
    ControlStop();
    g.controlStarted = false;
}

} // namespace

// ------------------------------------------------------------------------------------------------ entry points

bool OvrActive() { return g_active.load(); }

void OvrProcessInit(const std::wstring& proxyDir, bool realLoaded, const std::wstring& realPath, unsigned long loadError) {
    std::lock_guard<std::mutex> lk(g_lock);
    g.proxyDir = proxyDir;
    ProxyHome ph;
    if (!DiscoverProxyHome(proxyDir, ph)) return;   // no home: pure forwarder, nothing logged or written anywhere
    g.home = ph.home;
    g.gameName = ph.game;
    g.exeName = ExeBaseName();
    g.logPath = LogsDir(g.home) + L"\\eye_capture.log";
    elog::Init(g.logPath, false);
    LoadConfig(g.home, g.cfg);
    g.env = ReadCaptureEnv();
    if (!g.env.outputDir.empty()) g.cfg.outputDir = g.env.outputDir;
    if (!g.env.configOverrides.empty()) ApplyConfigOverrides(g.env.configOverrides, g.cfg);
    elog::SetLevel(g.cfg.logLevel);
    elog::SetMirror(g.cfg.outputDir + L"\\eye_capture.log");
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LOGI("==== VR EyeCapture SteamVR proxy v%s loaded into %s (pid %lu) -- real %s %s", EYE_CAPTURE_VERSION,
         Narrow(exe).c_str(), (unsigned long)GetCurrentProcessId(), Narrow(realPath).c_str(),
         realLoaded ? "loaded" : ("NOT LOADED (error " + std::to_string(loadError) + ") -- the game gets VR init errors").c_str());
    LOGI("steamvr: proxy folder %s | home from %s%s%s", Narrow(proxyDir).c_str(), Narrow(ph.from).c_str(),
         ph.game.empty() ? "" : " | game ", Narrow(ph.game).c_str());
    LogCaptureConfig("home", g.home, g.cfg);
    if (g.env.autoRecordSec > 0) LOGI("XR_EYE_CAPTURE_AUTORECORD=%.1f s: recording starts at the first frame", g.env.autoRecordSec);
    if (!g.env.trayIpc)
        LOGI("XR_EYE_CAPTURE_TRAY_IPC=0: no control thread -- no hotkey, nothing published to the tray (test runs)");
    if (!g.env.requireTray.empty()) {
        g.cfg.requireTray = _wtoi(g.env.requireTray.c_str()) != 0;
        LOGI("XR_EYE_CAPTURE_REQUIRE_TRAY=%s overrides config.ini", g.cfg.requireTray ? "1" : "0");
    }
    g.liveShm = g.env.liveShmOverride;
    if (g.liveShm.empty()) g.liveShm = EYECAP_LIVE_SHM_NAME;
    else LOGI("XR_EYE_CAPTURE_TEST_LIVE_SHM=%s: live share block name (test override)", Narrow(g.liveShm).c_str());
    if (!g.cfg.steamvr.enabled || g.cfg.steamvr.mode == SteamVrMode::Off) {
        LOGI("%s: pure forwarder -- nothing is hooked or recorded in this game",
             g.cfg.steamvr.enabled ? "steamvr_mode = off" : "steamvr_enabled = 0");
        return;
    }
    LOGI("steamvr: steamvr_enabled 1 | steamvr_mode %s (a game with the proxy always records through it) | steamvr_pose_source %s | require_tray %d | supported: D3D11 + D3D12 textures, IVRCompositor_019.._028 "
         "(C++ and FnTable)", SteamVrModeKey(g.cfg.steamvr.mode), SteamVrPoseSourceKey(g.cfg.steamvr.poseSource), g.cfg.requireTray ? 1 : 0);
    if (g.cfg.indicator.enabled && g.cfg.steamvr.indicator)
        LOGI("indicator: on (vr_indicator = 1, steamvr_indicator = 1) -- SteamVR overlay dot in the headset while recording at (%.2f, %.2f, %.2f) m from "
             "the head, %.3f m, opacity %.2f%s; never in the recording", g.cfg.indicator.x, g.cfg.indicator.y, g.cfg.indicator.z,
             g.cfg.indicator.size, g.cfg.indicator.opacity, g.cfg.indicator.live ? ", violet while a live viewer receives frames" : "");
    else
        LOGI("indicator: off (%s)", g.cfg.indicator.enabled ? "steamvr_indicator = 0" : "vr_indicator = 0");
    g.active = true;
    g_active = true;
}

void OvrOnInit(const char* fn, vr::EVRApplicationType appType, vr::EVRInitError err, uint32_t token) {
    bool startControl = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g.active) return;
        if (err != vr::VRInitError_None) {
            LOGW("steamvr: %s(app type %d) returned error %d -- nothing to record", fn, (int)appType, (int)err);
            return;
        }
        if (!ClaimProcess()) return;   // another VR EyeCapture proxy in this process records it
        g.inited = true;
        g.armed = !g.cfg.requireTray || EyeCapTrayRunning();
        if (!g.armed)
            LOGI("steamvr: %s(app type %d) OK (token %u) -- EyeCaptureTray not running, proxy dormant for this VR run "
                 "(set require_tray=0 in config.ini to always arm)", fn, (int)appType, token);
        else
            LOGI("steamvr: %s(app type %d) OK (token %u) -- armed", fn, (int)appType, token);
        startControl = g.armed;
    }
    if (startControl) EnsureControl();
}

void* OvrOnInterface(const char* name, void* iface, PFN_VR_GetGenericInterface realGet) {
    if (!iface || !name) return iface;
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g.active) return iface;
    if (!g.realGet) g.realGet = realGet;
    bool fnTable = false, isComp = false;
    const int v = FindCompositorVersion(name, &fnTable, &isComp);
    if (!isComp) {
        LOGD("steamvr: interface %s -> %p (not hooked)", name, iface);
        return iface;
    }
    if (v < 0) {
        bool seen = false;
        for (const std::string& s : g.unknownLogged) if (s == name) seen = true;
        if (!seen) {
            g.unknownLogged.push_back(name);
            LOGW("steamvr: %s is a version this proxy does not know -- forwarded untouched, this game is NOT recorded", name);
        }
        return iface;
    }
    if (!g.armed) {
        LOGI("steamvr: %s -> %p (dormant: not hooked)", name, iface);
        return iface;
    }
    if (!g.inited) {
        LOGI("steamvr: %s -> %p (not initialized: not hooked)", name, iface);
        return iface;
    }
    const CompositorVersion& cv = kCompositorVersions[v];
    if (fnTable) {
        void* const* real = static_cast<void* const*>(iface);
        uint32_t want = cv.methodCount + kFnTableCopyMargin;
        if (want > kFnTableMax) want = kFnTableMax;
        const uint32_t n = CopyFnTable(real, want, g_fnCopy[v]);
        if (n <= cv.slotSubmit) {
            LOGE("steamvr: %s: function table %p unreadable (%u entries) -- forwarded untouched, NOT recorded", name, iface, n);
            return iface;
        }
        g_fnRealTable[v] = iface;
        g_fnOrigWait[v] = g_fnCopy[v][cv.slotWaitGetPoses];
        g_fnOrigSubmit[v] = g_fnCopy[v][cv.slotSubmit];
        void* hw = nullptr;
        void* hs = nullptr;
        FnHooksFor(v, &hw, &hs);
        g_fnCopy[v][cv.slotWaitGetPoses] = hw;
        g_fnCopy[v][cv.slotSubmit] = hs;
        LOGI("steamvr: %s (FnTable %p): copied %u entries (%u methods + margin), WaitGetPoses (%u) / Submit (%u) hooked in "
             "the copy %p", name, iface, n, cv.methodCount, cv.slotWaitGetPoses, cv.slotSubmit, (void*)g_fnCopy[v]);
        return g_fnCopy[v];
    }
    void** vt = *static_cast<void***>(iface);
    const VtablePatchTable::Arm a = g_patches.ArmVtable(vt, v, cv.slotWaitGetPoses, cv.slotSubmit, (void*)&HookCpp_WaitGetPoses,
                                                        (void*)&HookCpp_Submit);
    switch (a) {
    case VtablePatchTable::Arm::AlreadyArmed: LOGD("steamvr: %s -> %p (vtable %p already patched)", name, iface, (void*)vt); break;
    case VtablePatchTable::Arm::Full: LOGE("steamvr: %s: too many distinct compositor vtables -- not hooked", name); break;
    case VtablePatchTable::Arm::ProtectFailed:
        LOGE("steamvr: %s: VirtualProtect on vtable %p failed (%lu) -- NOT recorded", name, (void*)vt, GetLastError());
        break;
    default:
        LOGI("steamvr: %s (C++ object %p): vtable %p %s in place -- slot %u WaitGetPoses, slot %u Submit (%d record(s))", name, iface,
             (void*)vt, a == VtablePatchTable::Arm::Rearmed ? "patched again (same vtable as before the last shutdown)" : "patched",
             cv.slotWaitGetPoses, cv.slotSubmit, g_patches.Count());
        break;
    }
    return iface;
}

void OvrOnShutdown() {
    std::unique_ptr<Recorder> rec;
    std::unique_ptr<LiveShare> live;
    std::unique_ptr<IFrameSource> src;
    bool wasArmed = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g.active) return;
        wasArmed = g.armed;
        // unpatch first: the real shutdown unloads vrclient, which owns the patched vtables. The records keep their vtable
        // (restored = true): a call already inside a hook still finds the originals; a re-init re-arms the same record.
        int kept = 0;
        const int restored = g_patches.RestoreAll((void*)&HookCpp_WaitGetPoses, (void*)&HookCpp_Submit, &kept);
        if (restored || kept) LOGI("steamvr: shutdown -- %d vtable(s) restored%s", restored, kept ? ", some slots were patched over by someone else (left)" : "");
        if (g.ind) {   // before the device goes: the overlay's texture lives on it
            g.ind->Destroy();
            g.ind.reset();
        }
        g.indTracker = IndicatorTracker();
        rec = std::move(g.rec);
        live = std::move(g.live);
        src = std::move(g.src);
        g.dev.Reset();
        g.dev12.Reset();
        g.queue12.Reset();
        g.indDev.Reset();
        g.indDevTried = false;
        g.api = GfxApi::None;
        g.tex.clear();
        g.pend[0].have = g.pend[1].have = false;
        g.pairPose = WaitPose();
        g.autoDone = false;   // XR_EYE_CAPTURE_AUTORECORD records again in the next VR session (tests: init -> shutdown -> init)
        g.autoStopAt = 0;
        g.sys = nullptr; g.sysVer = nullptr; g.sysTried = false;
        g.inited = false;
        g.armed = false;
        g.srcFailed = false;
        g_startRequested = false;
        LOGI("steamvr: VR_ShutdownInternal after %llu frame pairs (Submits L %llu R %llu, replaced %llu, runtime errors %llu, "
             "WaitGetPoses %llu)", (unsigned long long)g.pairs, (unsigned long long)g.submits[0], (unsigned long long)g.submits[1],
             (unsigned long long)g.replaced, (unsigned long long)g.submitErrors, (unsigned long long)g_waitCalls.load());
    }
    if (rec) {
        const RecState st = rec->State();
        bool finalized = false;
        if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) {
            LOGI("steamvr: VR shutting down while recording -- finalizing the file");
            rec->Stop();
            if (!rec->WaitIdle(30000)) LOGE("finalize did not finish within 30 s");
            finalized = true;
        }
        if (wasArmed) {
            const RecorderStatus rs = rec->Status();
            if (!rs.message.empty() && (finalized || rs.message.compare(0, 6, "saved ") == 0))
                ControlPublishFinal(rs, g.cfg.outputDir);
        }
    }
    live.reset();   // its teardown waits for the last copy on the game's GPU
    rec.reset();
    src.reset();
    StopControl();
}

void OvrLogMissingExport(const char* name) {
    static std::mutex& m = *new std::mutex;
    static std::vector<std::string>& seen = *new std::vector<std::string>;
    std::lock_guard<std::mutex> lk(m);
    for (const std::string& s : seen) if (s == name) return;
    seen.push_back(name);
    LOGE("steamvr: the real openvr_api_real.dll is missing or has no %s -- the game gets an error / nullptr", name);
}
