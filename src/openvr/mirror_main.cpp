// EyeCaptureMirror.exe -- SteamVR "quick mode" (config.ini [steamvr] steamvr_mode = quick): records SteamVR's mirror texture
// with no per-game setup. The tray starts it while steamvr_mode = quick and vrserver.exe runs, and stops it (the named
// event Local\OpenXR-EyeCapture-MirrorQuit) when SteamVR exits or the mode changes.
//
//   * Loads SteamVR's OWN openvr_api.dll by full path (VR EyeCapture ships none): XR_EYE_CAPTURE_MIRROR_OPENVR_DLL (tests:
//     the mock), else <runtime>\bin\win64\openvr_api.dll of the first existing runtime in %LOCALAPPDATA%\openvr\
//     openvrpaths.vrpath (FindSteamVrOpenvrApi; tests: XR_EYE_CAPTURE_TEST_OPENVRPATHS = another .vrpath file). Not found:
//     one log line and exit code 4 (kExitNoSteamVr; the tray shows "Quick mode needs SteamVR installed" once and does not
//     restart the mirror until steamvr_mode or the SteamVR state changes). A DLL that exports EyeCaptureProxyVersion is refused.
//   * Exit codes: 0 done, 1 failed, 3 another EyeCaptureMirror runs (single-instance mutex), 4 SteamVR's DLL not found.
//   * VR_InitInternal2(VRApplication_Background): no scene, never starts SteamVR (VRInitError_Init_NoServerForBackgroundApp
//     while vrserver is not running: retried every 2 s).
//   * Interfaces, asked by exact version (logged at start): "IVRSystem_022" (C++; GetDXGIOutputInfo slot 7, the display
//     frequency property, PollNextEvent slot 29 for VREvent_Quit) and "FnTable:IVRCompositor_022" (the C table of that
//     version: GetCurrentSceneFocusProcess 22, GetMirrorTextureD3D11 33, ReleaseMirrorTextureD3D11 34; see
//     third_party/openvr/openvr.h). Older versions are deliberately used: their layout is fixed, the runtime keeps serving them.
//   * Its own D3D11 device on the adapter IVRSystem::GetDXGIOutputInfo names; GetMirrorTextureD3D11(Left / Right) once
//     (re-asked after an error), released at exit. One texture for both eyes (the same resource): left / right half.
//   * Paced by a high-resolution waitable timer at the display frequency (Prop_DisplayFrequency_Float, else 90 Hz): each tick
//     copies both eyes into the normal Recorder (flat SBS) and the live share. No head pose (hasPose = false): head
//     smoothing / symmetric_fov do not apply, and VR180 / VR360 are refused (logged; recorded flat).
//   * Hotkey, tray commands, require_tray, beeps and the status block through ControlStart, like the proxy
//     (api "SteamVR mirror"). It is "capable" (owns the tray status) only while no other live process owns it and the scene
//     application holds neither the VR EyeCapture proxy's marker (Local\OpenXR-EyeCapture-Proxy-<pid>) nor the OpenXR
//     layer's (Local\OpenXR-EyeCapture-Layer-<pid>): a game recorded per game always wins (the mirror finalizes a running
//     recording and stays idle). Only named objects are checked (OpenMutexW); the game process is never opened or read.
//   * Exits on VREvent_Quit, on the quit event, or when the tray app is gone (require_tray), finalizing a recording.
// Logs to <home>\logs\eye_capture.log (home = --home <dir>, else the exe's folder).
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shlobj.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "capture_common.h"
#include "config.h"
#include "control.h"
#include "frame_source.h"
#include "live_share.h"
#include "live_share_proto.h"
#include "log.h"
#include "openvr_hooks.h"
#include "recorder.h"
#include "shared_status.h"

namespace {

const wchar_t kQuitEventName[] = L"Local\\OpenXR-EyeCapture-MirrorQuit";
const wchar_t kSingleName[] = L"Local\\OpenXR-EyeCapture-Mirror";
const int kExitNoSteamVr = 4;   // SteamVR's openvr_api.dll not found (tray/tray.cpp kMirrorExitNoSteamVr)
const char kSystemVersion[] = "IVRSystem_022";
const char kCompositorFnTable[] = "FnTable:IVRCompositor_022";

struct Openvr {
    HMODULE dll = nullptr;
    PFN_VR_InitInternal2 init2 = nullptr;
    PFN_VR_ShutdownInternal shutdown = nullptr;
    PFN_VR_GetGenericInterface getIf = nullptr;
    vr::IVRSystem* sys = nullptr;
    void* const* comp = nullptr;   // FnTable:IVRCompositor_022
    bool inited = false;
};

struct Mirror {
    std::wstring home, exeName = L"SteamVR", logPath;
    Config cfg;
    CaptureEnv env;
    Openvr ovr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    void* srv[2] = {nullptr, nullptr};          // from GetMirrorTextureD3D11 (released with ReleaseMirrorTextureD3D11)
    ComPtr<ID3D11Texture2D> tex[2];
    ComPtr<IDXGIKeyedMutex> km[2];              // the mirror texture is shared with a keyed mutex: key 0 around every copy
    SwapchainInfo info[2];
    bool combined = false, srgb = true;
    ULONGLONG nextMirrorTry = 0;
    std::unique_ptr<IFrameSource> src;
    std::unique_ptr<Recorder> rec;
    std::unique_ptr<LiveShare> live;
    float hz = 90.0f;
    bool yielding = false;                      // a per-game recorder (proxy / OpenXR layer) has priority
    std::string yieldWhy;
    uint64_t frames = 0;
    bool loggedDome = false, loggedSize = false;
    bool autoDone = false;
    int64_t autoStopAt = 0;
};

std::mutex& g_lock = *new std::mutex;
Mirror& g = *new Mirror;
std::atomic<bool> g_startRequested{false};

std::string N(const std::wstring& w) { return elog::Narrow(w); }

// quick mode records flat only, without a head pose
void ClampQuickMode(Config& c, bool log) {
    if (c.dome.projection != DomeProjection::Flat) {
        if (log && !g.loggedDome) {
            g.loggedDome = true;
            LOGW("mirror: quick mode records flat side-by-side only -- projection = %s is ignored (VR180 / VR360 need per-game "
                 "recording: SteamVR games... in the tray menu)", c.dome.projection == DomeProjection::Vr180 ? "vr180" : "vr360");
        }
        c.dome.projection = DomeProjection::Flat;
    }
    c.head.smoothing = 0.0f;
    c.head.symmetricFov = false;   // no per-frame pose / fov from the mirror texture
}

CaptureSnapshot Snapshot() {
    CaptureSnapshot s;
    std::lock_guard<std::mutex> lk(g_lock);
    s.game = g.exeName;
    s.outputDir = g.cfg.outputDir;
    if (g.src && g.rec && g.tex[0] && !g.yielding) {
        s.capable = true;
        s.rec = g.rec->Status();
        if (g_startRequested.load() && s.rec.state == RecState::Failed) s.rec.state = RecState::Idle;
        s.api = L"SteamVR mirror";
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

std::wstring ExeDir() {
    wchar_t p[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool LoadOpenvr(const std::wstring& path) {
    g.ovr.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g.ovr.dll) {
        LOGE("mirror: cannot load %s (error %lu)", N(path).c_str(), GetLastError());
        return false;
    }
    if (GetProcAddress(g.ovr.dll, EYECAP_PROXY_VERSION_EXPORT)) {
        LOGE("mirror: %s is the VR EyeCapture proxy, not Valve's openvr_api.dll -- refused", N(path).c_str());
        FreeLibrary(g.ovr.dll);
        g.ovr.dll = nullptr;
        return false;
    }
    g.ovr.init2 = (PFN_VR_InitInternal2)GetProcAddress(g.ovr.dll, "VR_InitInternal2");
    g.ovr.shutdown = (PFN_VR_ShutdownInternal)GetProcAddress(g.ovr.dll, "VR_ShutdownInternal");
    g.ovr.getIf = (PFN_VR_GetGenericInterface)GetProcAddress(g.ovr.dll, "VR_GetGenericInterface");
    if (!g.ovr.init2 || !g.ovr.shutdown || !g.ovr.getIf) {
        LOGE("mirror: %s lacks VR_InitInternal2 / VR_ShutdownInternal / VR_GetGenericInterface", N(path).c_str());
        return false;
    }
    wchar_t full[MAX_PATH * 2] = L"";
    GetModuleFileNameW(g.ovr.dll, full, MAX_PATH * 2);
    LOGI("mirror: openvr_api.dll = %s", N(full).c_str());
    return true;
}

// Background init + the two interfaces. false = not yet (no server) or failed (logged once per kind).
bool InitOpenvr() {
    static int lastErr = -1;
    vr::EVRInitError e = vr::VRInitError_None;
    g.ovr.init2(&e, vr::VRApplication_Background, nullptr);
    if (e != vr::VRInitError_None) {
        if ((int)e != lastErr) {
            lastErr = (int)e;
            LOGI("mirror: VR_InitInternal2(Background) -> error %d%s -- retrying every 2 s", (int)e,
                 e == vr::VRInitError_Init_NoServerForBackgroundApp ? " (SteamVR is not running)" : "");
        }
        return false;
    }
    g.ovr.inited = true;
    vr::EVRInitError e1 = vr::VRInitError_None, e2 = vr::VRInitError_None;
    g.ovr.sys = static_cast<vr::IVRSystem*>(g.ovr.getIf(kSystemVersion, &e1));
    g.ovr.comp = static_cast<void* const*>(g.ovr.getIf(kCompositorFnTable, &e2));
    LOGI("mirror: VR_InitInternal2(Background) OK | %s -> %p (error %d) | %s -> %p (error %d)", kSystemVersion, (void*)g.ovr.sys,
         (int)e1, kCompositorFnTable, (void*)g.ovr.comp, (int)e2);
    if (!g.ovr.sys || !g.ovr.comp) {
        LOGE("mirror: the runtime does not offer %s / %s -- quick mode cannot record", kSystemVersion, kCompositorFnTable);
        return false;
    }
    float hz = 0;
    int perr = 0;
    const SystemVersion sv = {kSystemVersion, 2, 4, 22};
    if (SysGetDisplayHz(g.ovr.sys, sv, &hz, &perr)) g.hz = hz;
    LOGI("mirror: display %.2f Hz%s", g.hz, hz > 0 ? "" : " (not reported: 90 assumed)");
    return true;
}

bool CreateDevice() {
    int32_t adapterIndex = -1;
    g.ovr.sys->GetDXGIOutputInfo(&adapterIndex);
    ComPtr<IDXGIFactory1> f;
    ComPtr<IDXGIAdapter1> ad;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && adapterIndex >= 0) f->EnumAdapters1((UINT)adapterIndex, &ad);
    DXGI_ADAPTER_DESC1 ds{};
    if (ad) ad->GetDesc1(&ds);
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(ad.Get(), ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
    if (FAILED(hr)) {
        LOGE("mirror: D3D11CreateDevice failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    LOGI("mirror: D3D11 device on adapter %d (%s, LUID %08lx:%08lx) from IVRSystem::GetDXGIOutputInfo", adapterIndex,
         ad ? N(ds.Description).c_str() : "default", (unsigned long)ds.AdapterLuid.HighPart, (unsigned long)ds.AdapterLuid.LowPart);
    g.src = CreateD3D11FrameSource(g.dev.Get());
    if (!g.src) { LOGE("mirror: no D3D11 frame source"); return false; }
    g.rec = std::make_unique<Recorder>();
    if (g.cfg.liveShare) {
        LiveShareParams lp;
        lp.api = GfxApi::D3D11;
        lp.dev11 = g.dev.Get();
        lp.game = g.exeName;
        lp.shmName = g.env.liveShmOverride.empty() ? std::wstring(EYECAP_LIVE_SHM_NAME) : g.env.liveShmOverride;
        g.live = std::make_unique<LiveShare>(lp);
    }
    return true;
}

void ReleaseMirror() {
    const auto rel = g.ovr.comp ? (vr::PFN_FnReleaseMirrorTextureD3D11)g.ovr.comp[vr::kFn022_ReleaseMirrorTextureD3D11] : nullptr;
    for (int e = 0; e < 2; ++e) {
        g.km[e].Reset();
        g.tex[e].Reset();
        if (g.srv[e] && rel) rel(g.srv[e]);
        g.srv[e] = nullptr;
    }
}

// Under g_lock. Both eyes' mirror textures (once; after a failure again every second).
bool EnsureMirror() {
    if (g.tex[0]) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < g.nextMirrorTry) return false;
    g.nextMirrorTry = now + 1000;
    const auto get = (vr::PFN_FnGetMirrorTextureD3D11)g.ovr.comp[vr::kFn022_GetMirrorTextureD3D11];
    for (int e = 0; e < 2; ++e) {
        const vr::EVRCompositorError r = get((vr::EVREye)e, g.dev.Get(), &g.srv[e]);
        if (r != vr::VRCompositorError_None || !g.srv[e]) {
            static int logged = 0;
            if (logged++ < 3) LOGW("mirror: GetMirrorTextureD3D11(%s) -> error %d", e ? "right" : "left", (int)r);
            g.srv[e] = nullptr;
            ReleaseMirror();
            return false;
        }
        ComPtr<ID3D11Resource> res;
        static_cast<ID3D11ShaderResourceView*>(g.srv[e])->GetResource(&res);
        if (!res || FAILED(res.As(&g.tex[e]))) {
            LOGE("mirror: the mirror SRV has no 2D texture");
            ReleaseMirror();
            return false;
        }
    }
    D3D11_TEXTURE2D_DESC d[2]{};
    g.tex[0]->GetDesc(&d[0]);
    g.tex[1]->GetDesc(&d[1]);
    g.combined = g.tex[0].Get() == g.tex[1].Get();
    for (int e = 0; e < (g.combined ? 1 : 2); ++e)
        if (d[e].MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) g.tex[e].As(&g.km[e]);
    g.srgb = SubmitIsSrgb(vr::ColorSpace_Auto, d[0].Format);
    for (int e = 0; e < 2; ++e) {
        g.info[e] = SwapchainInfo();
        g.info[e].format = (int64_t)TypedCaptureFormat(d[e].Format, g.srgb);
        g.info[e].width = d[e].Width;
        g.info[e].height = d[e].Height;
        g.info[e].arraySize = d[e].ArraySize ? d[e].ArraySize : 1;
        g.info[e].sampleCount = d[e].SampleDesc.Count ? d[e].SampleDesc.Count : 1;
    }
    LOGI("mirror: mirror textures: left %ux%u %s, right %ux%u %s%s%s", d[0].Width, d[0].Height, DxgiFormatName(d[0].Format),
         d[1].Width, d[1].Height, DxgiFormatName(d[1].Format), g.combined ? " -- ONE texture for both eyes: left / right half" : "",
         (d[0].MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) ? " (keyed mutex)" : "");
    return true;
}

void Views(EyeView v[2]) {
    for (int e = 0; e < 2; ++e) {
        v[e] = EyeView();
        v[e].image = g.tex[e].Get();
        v[e].sc = &g.info[e];
        v[e].swapchainKey = g.tex[e].Get();
        v[e].w = g.info[e].width;
        v[e].h = g.info[e].height;
        if (g.combined) {
            v[e].w = g.info[e].width / 2;
            v[e].x = e ? (int32_t)v[e].w : 0;
        }
    }
}

void StartRecording(EyeView L, EyeView R) {
    const DXGI_FORMAT stage = StageFormatForDxgi((DXGI_FORMAT)L.sc->format);
    if (stage == DXGI_FORMAT_UNKNOWN) {
        LOGE("mirror: texture format %s is not supported for capture", DxgiFormatName((DXGI_FORMAT)L.sc->format));
        return;
    }
    ReloadConfigForRecording(g.home, g.cfg);
    ClampQuickMode(g.cfg, true);
    bool srgb = g.srgb;
    if (g.cfg.gamma == GammaMode::Srgb) srgb = true;
    else if (g.cfg.gamma == GammaMode::Linear) srgb = false;
    const char* from = "config";
    const unsigned fps = PickFps(g.cfg, 0, 0, g.hz, "Prop_DisplayFrequency_Float", &from);
    SHCreateDirectoryExW(nullptr, g.cfg.outputDir.c_str(), nullptr);
    RecorderSettings rs;
    rs.path = g.cfg.outputDir + L"\\" + RecordingFileName(g.exeName, DomeProjection::Flat);
    FillRecorderSettings(g.cfg, L, R, fps, rs);
    const UINT W = L.w + R.w, H = L.h > R.h ? L.h : R.h;
    LOGI("record: start %s | SteamVR mirror (quick mode) | %ux%u SBS (eyes %ux%u + %ux%u) | %u fps from %s | %s",
         N(rs.path).c_str(), W, H, L.w, L.h, R.w, R.h, fps, from, srgb ? "sRGB-encoded" : "linear");
    if (!g.rec->Start(g.src.get(), rs, W, H, L.w, stage, srgb)) LOGW("record: previous recording still finalizing -- start ignored");
}

// Under g_lock: one tick of the timer.
void Tick() {
    ++g.frames;
    bool wantStart = g_startRequested.exchange(false);
    if (g.yielding) {
        if (wantStart) LOGI("mirror: start ignored -- %s records this game", g.yieldWhy.c_str());
        return;
    }
    if (!EnsureMirror()) return;
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
    EyeView v[2];
    Views(v);
    if (g.cfg.swapEyes) std::swap(v[0], v[1]);
    const RecState rs = g.rec->State();
    if (wantStart && rs != RecState::Starting && rs != RecState::Recording) StartRecording(v[0], v[1]);
    const bool rec = g.rec->State() == RecState::Recording;
    if (!rec && !g.live) return;
    // a keyed-mutex mirror texture: key 0 for the copies (10 ms; a busy compositor costs this tick, not a stall)
    int held = 0;
    for (; held < 2; ++held) {
        if (!g.km[held]) continue;
        const HRESULT hr = g.km[held]->AcquireSync(0, 10);
        if (hr != S_OK) {
            static int logged = 0;
            if (logged++ < 3) LOGW("mirror: AcquireSync(0) on the mirror texture -> 0x%08lx -- frame skipped", (unsigned long)hr);
            for (int k = 0; k < held; ++k) if (g.km[k]) g.km[k]->ReleaseSync(0);
            return;
        }
    }
    const int64_t ts = QpcNow100ns();
    if (rec) g.rec->SubmitFrame(ts, v[0], v[1], 0, nullptr);
    if (g.live) {
        bool s = g.srgb;
        if (g.cfg.gamma == GammaMode::Srgb) s = true;
        else if (g.cfg.gamma == GammaMode::Linear) s = false;
        g.live->Frame(v[0], v[1], StageFormatForDxgi((DXGI_FORMAT)v[0].sc->format), s, ts, g.cfg.head, false, nullptr);
    }
    for (int k = 0; k < 2; ++k) if (g.km[k]) g.km[k]->ReleaseSync(0);
}

// Once a second: does a per-game recorder have priority? (another live owner of the status block, or the scene app holds
// the proxy's / the OpenXR layer's per-process marker -- a named mutex, src/shared_status.h; nothing is read from the game)
void CheckYield(EyeCapShared* shm) {
    std::string why;
    if (shm) {
        EyeCapShared st{};
        if (EyeCapReadStatus(shm, st) && st.ownerPid && st.ownerPid != GetCurrentProcessId() && EyeCapProcessAlive(st.ownerPid))
            why = "pid " + std::to_string(st.ownerPid) + " (" + N(st.api) + ")";
    }
    if (why.empty() && g.ovr.comp) {
        const DWORD scene = ((vr::PFN_FnGetCurrentSceneFocusProcess)g.ovr.comp[vr::kFn022_GetCurrentSceneFocusProcess])();
        if (scene && scene != GetCurrentProcessId()) {
            if (EyeCapMarkerExists(EYECAP_PROXY_MARKER_PREFIX, scene)) why = "the per-game SteamVR recorder in pid " + std::to_string(scene);
            else if (EyeCapMarkerExists(EYECAP_LAYER_MARKER_PREFIX, scene)) why = "the OpenXR layer in pid " + std::to_string(scene);
        }
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const bool y = !why.empty();
    if (y != g.yielding) {
        LOGI("mirror: %s", y ? ("idle -- " + why + " records this game (per game wins)").c_str() : "active again (no per-game recorder)");
        if (y && g.rec) {
            const RecState s = g.rec->State();
            if (s == RecState::Starting || s == RecState::Recording) {
                LOGI("mirror: stopping the mirror recording");
                g.rec->Stop();
            }
        }
    }
    g.yielding = y;
    g.yieldWhy = why;
}

bool PollQuit() {
    if (!g.ovr.sys) return false;
    vr::VREvent_t ev{};
    for (int i = 0; i < 64 && g.ovr.sys->PollNextEvent(&ev, sizeof(ev)); ++i) {
        if (ev.eventType == vr::VREvent_Quit) { LOGI("mirror: VREvent_Quit -- SteamVR is closing"); return true; }
    }
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, kSingleName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 3;
    // --home <dir> (the tray's folder: config.ini, logs, captures); default the exe's folder
    std::wstring args = cmdLine ? cmdLine : L"";
    g.home = ExeDir();
    {
        const size_t k = args.find(L"--home ");
        if (k != std::wstring::npos) {
            std::wstring h = args.substr(k + 7);
            while (!h.empty() && h.front() == L' ') h.erase(0, 1);
            if (!h.empty() && h.front() == L'"') { const size_t q = h.find(L'"', 1); h = h.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1); }
            else { const size_t sp = h.find(L' '); if (sp != std::wstring::npos) h = h.substr(0, sp); }
            while (!h.empty() && (h.back() == L'\\' || h.back() == L'/')) h.pop_back();
            if (!h.empty()) g.home = h;
        }
    }
    g.logPath = g.home + L"\\logs\\eye_capture.log";
    elog::Init(g.logPath, false);
    LoadConfig(g.home, g.cfg);
    g.env = ReadCaptureEnv();
    if (!g.env.outputDir.empty()) g.cfg.outputDir = g.env.outputDir;
    if (!g.env.configOverrides.empty()) ApplyConfigOverrides(g.env.configOverrides, g.cfg);
    if (!g.env.requireTray.empty()) g.cfg.requireTray = _wtoi(g.env.requireTray.c_str()) != 0;
    elog::SetLevel(g.cfg.logLevel);
    elog::SetMirror(g.cfg.outputDir + L"\\eye_capture.log");
    LOGI("==== VR EyeCapture SteamVR mirror (quick mode) v%s (pid %lu), home %s", EYE_CAPTURE_VERSION,
         (unsigned long)GetCurrentProcessId(), N(g.home).c_str());
    LogCaptureConfig("home", g.home, g.cfg);
    if (g.cfg.steamvr.mode != SteamVrMode::Quick || !g.cfg.steamvr.enabled) {
        LOGI("mirror: steamvr_mode = %s%s -- nothing to do", SteamVrModeKey(g.cfg.steamvr.mode), g.cfg.steamvr.enabled ? "" : ", steamvr_enabled = 0");
        return 0;
    }
    ClampQuickMode(g.cfg, true);
    std::wstring dllPath = EnvW(L"XR_EYE_CAPTURE_MIRROR_OPENVR_DLL");
    if (!dllPath.empty()) {
        LOGI("XR_EYE_CAPTURE_MIRROR_OPENVR_DLL=%s (test override)", N(dllPath).c_str());
    } else {   // SteamVR's own copy: <runtime>\bin\win64\openvr_api.dll from %LOCALAPPDATA%\openvr\openvrpaths.vrpath
        std::wstring vrpath = EnvW(L"XR_EYE_CAPTURE_TEST_OPENVRPATHS");   // tests: another openvrpaths.vrpath
        if (!vrpath.empty()) LOGI("XR_EYE_CAPTURE_TEST_OPENVRPATHS=%s (test override)", N(vrpath).c_str());
        else vrpath = DefaultOpenvrPathsFile();
        std::wstring why;
        dllPath = FindSteamVrOpenvrApi(vrpath, &why);
        if (dllPath.empty()) {
            LOGE("mirror: SteamVR's openvr_api.dll not found -- %s. Quick mode needs SteamVR installed (exit code %d)", N(why).c_str(),
                 kExitNoSteamVr);
            return kExitNoSteamVr;
        }
        LOGI("mirror: SteamVR's openvr_api.dll found through %s", N(vrpath).c_str());
    }
    if (!LoadOpenvr(dllPath)) return 1;

    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
    HANDLE map = nullptr;
    const std::wstring testShm = EnvW(L"XR_EYE_CAPTURE_TEST_SHM");   // tests: a private status block (control.cpp uses it too)
    EyeCapShared* shm = testShm.empty() ? EyeCapOpenShared(&map) : EyeCapOpenShared(&map, testShm.c_str());
    if (!testShm.empty()) LOGI("XR_EYE_CAPTURE_TEST_SHM=%s: private status block (test)", N(testShm).c_str());
    auto shouldQuit = [&] {
        if (quit && WaitForSingleObject(quit, 0) == WAIT_OBJECT_0) { LOGI("mirror: quit requested by the tray"); return true; }
        if (g.cfg.requireTray && !EyeCapTrayRunning()) { LOGI("mirror: EyeCaptureTray is not running (require_tray) -- exiting"); return true; }
        return false;
    };
    // wait for SteamVR
    while (!InitOpenvr()) {
        if (g.ovr.inited) { g.ovr.shutdown(); g.ovr.inited = false; return 1; }
        if (shouldQuit()) return 0;
        Sleep(2000);
    }
    int rc = 0;
    if (!CreateDevice()) rc = 1;
    if (rc == 0) {
        if (g.env.trayIpc)
            ControlStart(g.cfg, g.logPath, g.cfg.loadedFrom.empty() ? ConfigPath(g.home) : g.cfg.loadedFrom,
                         ControlHooks{Snapshot, RequestStart, RequestStop});
        else
            LOGI("XR_EYE_CAPTURE_TRAY_IPC=0: no control thread -- no hotkey, nothing published to the tray (test runs)");
        if (g.env.autoRecordSec > 0) LOGI("XR_EYE_CAPTURE_AUTORECORD=%.1f s: recording starts at the first mirror frame", g.env.autoRecordSec);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        const LONGLONG period100ns = (LONGLONG)(1e7 / (g.hz > 20 ? g.hz : 90.0f));
        LARGE_INTEGER due;
        due.QuadPart = -period100ns;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        int64_t next = QpcNow100ns() + period100ns;
        ULONGLONG lastCheck = 0;
        for (;;) {
            WaitForSingleObject(timer, 100);
            const int64_t now = QpcNow100ns();
            next += period100ns;
            if (next < now) next = now + period100ns;   // fell behind: no burst
            due.QuadPart = -(next - now);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            if (GetTickCount64() - lastCheck >= 1000) {
                lastCheck = GetTickCount64();
                if (shouldQuit() || PollQuit()) break;
                CheckYield(shm);
            } else if (g_startRequested.load() || (g.env.autoRecordSec > 0 && !g.autoDone)) {
                CheckYield(shm);   // a start is about to happen: decide with the current owner, not one up to 1 s old
            }
            std::lock_guard<std::mutex> lk(g_lock);
            Tick();
        }
        if (timer) CloseHandle(timer);
    }
    // finalize
    std::unique_ptr<Recorder> rec;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        rec = std::move(g.rec);
    }
    if (rec) {
        const RecState st = rec->State();
        bool finalized = false;
        if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) {
            LOGI("mirror: exiting while recording -- finalizing the file");
            rec->Stop();
            if (!rec->WaitIdle(30000)) LOGE("finalize did not finish within 30 s");
            finalized = true;
        }
        const RecorderStatus rs = rec->Status();
        if (g.env.trayIpc && !rs.message.empty() && (finalized || rs.message.compare(0, 6, "saved ") == 0)) ControlPublishFinal(rs, g.cfg.outputDir);
    }
    g.live.reset();
    rec.reset();
    {
        std::lock_guard<std::mutex> lk(g_lock);
        ReleaseMirror();
        g.src.reset();
    }
    if (g.env.trayIpc) ControlStop();
    LOGI("mirror: %llu ticks; shutting down", (unsigned long long)g.frames);
    if (g.ovr.inited) g.ovr.shutdown();
    g.ctx.Reset();
    g.dev.Reset();
    if (shm) UnmapViewOfFile(shm);
    if (map) CloseHandle(map);
    if (quit) CloseHandle(quit);
    if (single) CloseHandle(single);
    return rc;
}
