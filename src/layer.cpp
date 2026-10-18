// layer -- XR_APILAYER_JRDEVO_eye_capture: an implicit OpenXR API layer that records both eye images of any OpenXR
// game into a side-by-side 3D MP4.
//
// Call chain: loader -> [this layer] -> other layers -> runtime. Everything not hooked is forwarded untouched (the
// layer's xrGetInstanceProcAddr returns the next layer's pointers). Hooked:
//   xrCreateSession        find the graphics binding (D3D11 / D3D12 / Vulkan / OpenGL) -> IFrameSource
//   xrDestroySession       stop + finalize the recording, release
//   xrCreateSwapchain / xrDestroySwapchain / xrEnumerateSwapchainImages   track images + format/size/array/samples
//   xrAcquireSwapchainImage / xrReleaseSwapchainImage                     track the most recently released image
//   xrEndFrame             first projection layer: views[0] = left, views[1] = right (images + pose + fov)
//                          -> Recorder::SubmitFrame, then forward
//                          + in-headset recording indicator: one quad layer appended AFTER the capture (indicator.h)
//                          + record_overlays: quad / cylinder / equirect / cube layers around the projection layer (overlay.h)
//                          + live_share: the same L / R views to a live viewer (OBS plugin) when one is watching (live_share.h)
//   xrCreateReferenceSpace / xrDestroySpace   track VIEW spaces (overlay layers in them follow the submitted head)
//   xrDestroyInstance      clean up
// Rule: any internal failure logs and passes through. The game's calls are always forwarded and their results returned
// unchanged; capture work in xrEndFrame is wrapped in SEH so a fault disables capture instead of crashing the game.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <shlobj.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "capture_common.h"
#include "config.h"
#include "control.h"
#include "frame_source.h"
#include "indicator.h"
#include "live_share.h"
#include "live_share_proto.h"
#include "log.h"
#include "overlay.h"
#include "recorder.h"
#include "shared_status.h"
#include "yt_layout.h"

#define LAYER_NAME "XR_APILAYER_JRDEVO_eye_capture"

namespace {

// ------------------------------------------------------------------------------------------------ state

struct InstanceData {
    XrInstance handle = XR_NULL_HANDLE;
    PFN_xrGetInstanceProcAddr nextGipa = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrGetInstanceProperties GetInstanceProperties = nullptr;
    PFN_xrGetDisplayRefreshRateFB GetDisplayRefreshRateFB = nullptr;   // only if the app enabled the extension
    // used by the in-headset indicator (called directly on the next layer: our own hooks never see these objects)
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrGetSystemProperties GetSystemProperties = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;           // record_overlays: overlay layer spaces -> the projection space
    std::string runtimeName;
    std::string appName;
};

struct SessionData;

struct SwapchainData {
    XrSwapchain handle = XR_NULL_HANDLE;
    SessionData* session = nullptr;
    SwapchainInfo info;
    std::vector<void*> images;          // ID3D11Texture2D* / ID3D12Resource* / VkImage
    std::deque<uint32_t> acquired;      // acquired, not yet released (release releases the oldest)
    int lastReleased = -1;
};

struct SessionData {
    XrSession handle = XR_NULL_HANDLE;
    InstanceData* inst = nullptr;
    GfxApi api = GfxApi::None;
    std::unique_ptr<IFrameSource> src;
    std::unique_ptr<Recorder> rec;
    std::unique_ptr<LiveShare> live;     // live_share = 1, armed sessions with a frame source (idle until a consumer appears)
    bool armed = false;                 // tray running (or require_tray=0) when the session was created; else dormant
    bool faulted = false;
    uint64_t frames = 0;
    // submitted-frame period estimate (displayTime deltas)
    PeriodEstimator period;
    // clock anchor of the current recording
    VideoClockAnchor clock;
    // layout logging
    std::string layoutSig;
    int layoutLogs = 0;
    bool loggedNoProjection = false, loggedViewCount = false, loggedNoImage = false;
    // autorecord (XR_EYE_CAPTURE_AUTORECORD=<seconds>, for tests / automation)
    bool autoDone = false;
    int64_t autoStopAt = 0;
    bool auto2Done = false;
    int64_t auto2At = 0;                 // XR_EYE_CAPTURE_AUTORECORD2: second recording (config reload test)
    // in-headset recording indicator (armed sessions with a recorder only; created on first use)
    IndicatorSettings indCfg;
    IndicatorTracker indTracker;
    std::unique_ptr<VrIndicator> ind;
    ComPtr<ID3D11Device> dev11;          // D3D11 sessions: the game's device (indicator uploads)
    uint32_t maxLayers = 16;             // XrSystemGraphicsProperties::maxLayerCount
    bool indOff = false, loggedIndAppend = false, loggedIndLimit = false;
    // record_overlays: overlay layers already logged this session (one line per layer signature, capped)
    std::vector<std::string> ovSeen;
    bool loggedOvOff = false;
};

// Heap-allocated and never freed on purpose: if the game exits without xrDestroySession, static destructors at DLL
// unload would run ~Recorder (which waits for worker threads the OS has already killed) and COM releases on a dying
// device. Leaking them at process exit is the safe choice.
std::mutex& g_lock = *new std::mutex;
auto& g_instances = *new std::unordered_map<XrInstance, std::unique_ptr<InstanceData>>;
auto& g_sessions = *new std::unordered_map<XrSession, std::unique_ptr<SessionData>>;
auto& g_swapchains = *new std::unordered_map<XrSwapchain, std::unique_ptr<SwapchainData>>;
// record_overlays: VIEW reference spaces created through our xrCreateReferenceSpace hook (by the game or a layer / mod
// above us, e.g. the DLAA injector's head-locked panel space), with their poseInReferenceSpace.
struct ViewSpaceData { XrSession session; InstanceData* inst; float q[4], p[3]; };
auto& g_viewSpaces = *new std::unordered_map<XrSpace, ViewSpaceData>;
SessionData* g_active = nullptr;              // newest capture-capable session (hotkey / tray act on it)
std::atomic<bool> g_startRequested{false};
std::atomic<uint64_t> g_faults{0};
PFN_xrGetInstanceProcAddr g_anyNextGipa = nullptr;
std::mutex& g_ctlLock = *new std::mutex;      // serialises ControlStart / ControlStop (lock order: g_ctlLock -> g_lock)
void EnsureControl();                         // start the control thread (first armed session)
void StopControlIfIdle();                     // stop it when no armed session is left

// process-wide setup
std::once_flag g_initOnce;
Config g_cfg;
std::wstring g_dllDir, g_exeName, g_logPath;
double g_autoRecordSec = 0;
double g_autoRecord2Sec = 0;   // test-only XR_EYE_CAPTURE_AUTORECORD2=<s>: a second autorecord ~1 s after the first stopped
bool g_trayIpc = true;   // test-only XR_EYE_CAPTURE_TRAY_IPC=0: no control thread (no hotkey, nothing published to a tray)
std::wstring g_liveShm;  // live share block name (EYECAP_LIVE_SHM_NAME; test-only override XR_EYE_CAPTURE_TEST_LIVE_SHM)

// ------------------------------------------------------------------------------------------------ helpers

std::wstring ModuleDir() { return ModuleDirOf((const void*)&ModuleDir); }

void InitProcess() {
    g_dllDir = ModuleDir();
    g_exeName = ExeBaseName();
    g_logPath = LogsDir(g_dllDir) + L"\\eye_capture.log";
    elog::Init(g_logPath, false);
    LoadConfig(g_dllDir, g_cfg);
    const CaptureEnv env = ReadCaptureEnv();
    if (!env.outputDir.empty()) g_cfg.outputDir = env.outputDir;
    g_autoRecordSec = env.autoRecordSec;
    g_autoRecord2Sec = env.autoRecord2Sec;
    if (!env.configOverrides.empty()) ApplyConfigOverrides(env.configOverrides, g_cfg);   // test-only: "key=value;key=value"
    elog::SetLevel(g_cfg.logLevel);
    elog::SetMirror(g_cfg.outputDir + L"\\eye_capture.log");
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LOGI("==== " LAYER_NAME " v%s loaded into %s (pid %lu)", EYE_CAPTURE_VERSION, elog::Narrow(exe).c_str(),
         (unsigned long)GetCurrentProcessId());
    LogCaptureConfig("layer dll folder", g_dllDir, g_cfg);
    if (g_autoRecordSec > 0) LOGI("XR_EYE_CAPTURE_AUTORECORD=%.1f s: recording starts at the first frame", g_autoRecordSec);
    if (!env.trayIpc) {
        g_trayIpc = false;
        LOGI("XR_EYE_CAPTURE_TRAY_IPC=0: no control thread -- no hotkey, nothing published to the tray (test runs)");
    }
    if (!env.requireTray.empty()) {
        g_cfg.requireTray = _wtoi(env.requireTray.c_str()) != 0;
        LOGI("XR_EYE_CAPTURE_REQUIRE_TRAY=%s overrides config.ini", g_cfg.requireTray ? "1" : "0");
    }
    LOGI("require_tray %s: %s", g_cfg.requireTray ? "1" : "0",
         g_cfg.requireTray ? "sessions are armed only while EyeCaptureTray.exe is running" : "sessions are always armed");
    // Marker for EyeCaptureMirror (SteamVR quick mode): this process has the OpenXR layer, so the mirror leaves its picture to
    // us. A named mutex, never owned, kept open (leaked) for the life of the process (src/shared_status.h).
    const std::wstring marker = EYECAP_LAYER_MARKER_PREFIX + std::to_wstring(GetCurrentProcessId());
    if (!CreateMutexW(nullptr, FALSE, marker.c_str()))
        LOGW("cannot create the layer marker %s (error %lu)", elog::Narrow(marker).c_str(), GetLastError());
    g_liveShm = env.liveShmOverride;
    if (g_liveShm.empty()) g_liveShm = EYECAP_LIVE_SHM_NAME;
    else LOGI("XR_EYE_CAPTURE_TEST_LIVE_SHM=%s: live share block name (test override)", elog::Narrow(g_liveShm).c_str());
}

void EnsureInit() { std::call_once(g_initOnce, InitProcess); }

InstanceData* FindInstance(XrInstance i) {
    auto it = g_instances.find(i);
    return it == g_instances.end() ? nullptr : it->second.get();
}
SessionData* FindSession(XrSession s) {
    auto it = g_sessions.find(s);
    return it == g_sessions.end() ? nullptr : it->second.get();
}
SwapchainData* FindSwapchain(XrSwapchain s) {
    auto it = g_swapchains.find(s);
    return it == g_swapchains.end() ? nullptr : it->second.get();
}

const char* LayoutName(const EyeView& L, const EyeView& R) {
    if (L.swapchainKey != R.swapchainKey) return "TWO_SWAPCHAINS";
    if (L.arrayIndex != R.arrayIndex) return "TEXTURE_ARRAY (one swapchain, arrayIndex 0/1)";
    if (L.x != R.x || L.y != R.y) return "ONE_SWAPCHAIN_TWO_RECTS (wide texture)";
    return "SAME_IMAGE_SAME_RECT (both views identical -- recorded twice)";
}

std::string FormatName(GfxApi api, int64_t f) {
    if (api == GfxApi::D3D11 || api == GfxApi::D3D12) return DxgiFormatName((DXGI_FORMAT)f) + std::string("(") + std::to_string(f) + ")";
    return "format " + std::to_string(f);
}

// ------------------------------------------------------------------------------------------------ capture

void LogLayout(SessionData* sd, const XrFrameEndInfo* info, const XrCompositionLayerProjection* proj, const EyeView& L, const EyeView& R) {
    char sig[512];
    snprintf(sig, sizeof(sig), "%s | L %ux%u+%d+%d slice %u | R %ux%u+%d+%d slice %u", LayoutName(L, R), L.w, L.h, L.x, L.y,
             L.arrayIndex, R.w, R.h, R.x, R.y, R.arrayIndex);
    if (sd->layoutSig == sig) return;
    sd->layoutSig = sig;
    if (++sd->layoutLogs > 20) return;
    std::string kinds;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (!info->layers[i]) continue;
        const XrStructureType t = info->layers[i]->type;
        kinds += t == XR_TYPE_COMPOSITION_LAYER_PROJECTION ? "projection " : t == XR_TYPE_COMPOSITION_LAYER_QUAD ? "quad " : "other ";
    }
    LOGI("frame layout (frame %llu): %s", (unsigned long long)sd->frames, sig);
    LOGI("  left : %s %ux%u array %u samples %u | right: %s %ux%u array %u samples %u | layers this frame: %s| projection viewCount %u, space-layer flags 0x%llx",
         FormatName(sd->api, L.sc->format).c_str(), L.sc->width, L.sc->height, L.sc->arraySize, L.sc->sampleCount,
         FormatName(sd->api, R.sc->format).c_str(), R.sc->width, R.sc->height, R.sc->arraySize, R.sc->sampleCount,
         kinds.c_str(), proj->viewCount, (unsigned long long)proj->layerFlags);
}

void StartRecording(SessionData* sd, const EyeView& L, const EyeView& R) {
    DXGI_FORMAT stage = DXGI_FORMAT_UNKNOWN, stageR = DXGI_FORMAT_UNKNOWN;
    bool srgb = true, srgbR = true;
    if (!sd->src->StageFormat(*L.sc, &stage, &srgb) || !sd->src->StageFormat(*R.sc, &stageR, &srgbR)) {
        LOGE("record: swapchain format %s / %s is not supported for capture", FormatName(sd->api, L.sc->format).c_str(),
             FormatName(sd->api, R.sc->format).c_str());
        return;
    }
    if (stage != stageR) {
        LOGE("record: left and right eye swapchains have different formats (%s / %s) -- not supported",
             FormatName(sd->api, L.sc->format).c_str(), FormatName(sd->api, R.sc->format).c_str());
        return;
    }
    if (g_cfg.gamma == GammaMode::Srgb) srgb = true;
    else if (g_cfg.gamma == GammaMode::Linear) srgb = false;

    // frame rate: measured submit rate (what the encoder really gets), else the runtime's display refresh, else config
    const char* fpsFrom = "config";
    float refresh = 0;
    if (sd->inst->GetDisplayRefreshRateFB) sd->inst->GetDisplayRefreshRateFB(sd->handle, &refresh);
    const unsigned fps = PickFps(g_cfg, sd->period.periodNs, sd->period.samples, refresh, "XR_FB_display_refresh_rate", &fpsFrom);

    const std::wstring name = RecordingFileName(g_exeName, g_cfg.dome.projection);
    SHCreateDirectoryExW(nullptr, g_cfg.outputDir.c_str(), nullptr);

    RecorderSettings rs;
    rs.path = g_cfg.outputDir + L"\\" + name;
    FillRecorderSettings(g_cfg, L, R, fps, rs);
    const UINT W = L.w + R.w, H = L.h > R.h ? L.h : R.h;
    LOGI("record: start %s | %s | %ux%u SBS (eyes %ux%u + %ux%u) | %u fps from %s (display refresh %.1f, measured %.2f) | %s",
         elog::Narrow(rs.path).c_str(), GfxApiName(sd->api), W, H, L.w, L.h, R.w, R.h, fps, fpsFrom, refresh,
         sd->period.periodNs > 0 ? 1e9 / sd->period.periodNs : 0.0, srgb ? "sRGB-encoded" : "linear");
    sd->clock.Reset();
    if (!sd->rec->Start(sd->src.get(), rs, W, H, L.w, stage, srgb))
        LOGW("record: previous recording still finalizing -- start ignored");
}

int64_t VideoClock(SessionData* sd, XrTime displayTime) { return sd->clock.Map((int64_t)displayTime); }

// Re-read config.ini at the start of each recording (the tray's Settings window edits it): only keys that can change per
// recording are copied over; hotkey, require_tray, vr_indicator / indicator_* keep their game-start values
// (post_* belong to the tray). beeps goes to the control thread (ControlSetBeeps), before the start cue plays.
// Called with g_lock held, on the game thread, before the swap_eyes decision. (capture_common.cpp)
void ReloadConfigForRecording() { ::ReloadConfigForRecording(g_dllDir, g_cfg); }

// ------------------------------------------------------------------------------------------------ overlays

const char* LayerTypeName(XrStructureType t) {
    switch ((int)t) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return "projection";
    case XR_TYPE_COMPOSITION_LAYER_QUAD: return "quad";
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: return "cylinder";
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: return "equirect";
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: return "equirect2";
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: return "cube";
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB: return "XR_FB_passthrough layer";
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC: return "XR_HTC_passthrough layer";
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_ANDROID: return "XR_ANDROID passthrough layer";
    default: return "unknown layer type";
    }
}

// One log line per overlay layer signature (type + swapchain + space + outcome) and session, at most 40.
void OvNote(SessionData* sd, const std::string& sig, const char* fmt, ...) {
    for (const std::string& s : sd->ovSeen) if (s == sig) return;
    if (sd->ovSeen.size() >= 40) return;
    sd->ovSeen.push_back(sig);
    char b[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    LOGI("overlay: %s%s", b, sd->ovSeen.size() == 40 ? " (no more overlay lines this session)" : "");
}

// q = a * b (Hamilton product, x y z w); v rotated by q
void QuatMul(const float* a, const float* b, float* q) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    q[0] = x; q[1] = y; q[2] = z; q[3] = w;
}
void QuatRotate(const float* q, const float* v, float* out) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// An XrCompositionLayerColorScaleBiasKHR in a layer's next chain (at most 16 links walked), or nullptr.
const XrCompositionLayerColorScaleBiasKHR* FindColorScaleBias(const void* next) {
    const XrBaseInStructure* p = static_cast<const XrBaseInStructure*>(next);
    for (int i = 0; p && i < 16; ++i, p = p->next)
        if (p->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) return reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(p);
    return nullptr;
}

// record_overlays: the quad / cylinder / equirect / equirect2 / cube layers around the captured projection layer
// (index projIdx), back to front, with this frame's swapchain image and the pose located in the projection layer's space
// at displayTime (xrLocateSpace of the NEXT layer: our hooks are not involved). Layers BEFORE the projection layer are
// collected only when it has BLEND_TEXTURE_SOURCE_ALPHA (they show through its transparent pixels; out.layer[0 ..
// out.nBehind)); with an opaque projection they are hidden and skipped (logged). Our own indicator is appended to the
// frame only after the capture (Hook_EndFrame), so it is never in info->layers here. swapped = swap_eyes (output eye 0
// holds the right eye).
void CollectOverlays(SessionData* sd, const XrFrameEndInfo* info, uint32_t projIdx, const XrCompositionLayerProjection* proj,
                     bool swapped, OverlayFrame& out) {
    out.n = 0;
    out.nBehind = 0;
    const bool projBlend = (proj->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
    out.projUnpremultiplied = projBlend && (proj->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
    if (const XrCompositionLayerColorScaleBiasKHR* pc = FindColorScaleBias(proj->next)) {
        OvNote(sd, "projection colour scale bias",
               "the projection layer carries XrCompositionLayerColorScaleBiasKHR (scale %.2f %.2f %.2f %.2f, bias %.2f %.2f %.2f "
               "%.2f) -- not applied to the recorded eyes", pc->colorScale.r, pc->colorScale.g, pc->colorScale.b, pc->colorScale.a,
               pc->colorBias.r, pc->colorBias.g, pc->colorBias.b, pc->colorBias.a);
    }
    struct Located { XrSpace space; bool ok; XrPosef pose; XrResult r; uint64_t flags; };
    Located cache[kMaxOverlays];
    uint32_t nCache = 0;
    char sig[180];
    // The head implied by the two submitted views (in the projection space): VIEW space origin = the midpoint of the eye
    // positions, orientation = the halfway rotation of the two eye orientations (runtimes report the eyes at -+IPD/2
    // along the head's +X with the head's orientation, or symmetrically canted). Used for VIEW-space layers.
    struct { float q[4], p[3]; } head{};
    bool headOk = false;
    {
        const XrPosef& a = proj->views[0].pose;
        const XrPosef& b = proj->views[1].pose;
        float qa[4] = {a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w};
        float qb[4] = {b.orientation.x, b.orientation.y, b.orientation.z, b.orientation.w};
        if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0) for (float& c : qb) c = -c;
        float n = 0;
        for (int k = 0; k < 4; ++k) { head.q[k] = qa[k] + qb[k]; n += head.q[k] * head.q[k]; }   // nlerp at 0.5
        n = std::sqrt(n);
        head.p[0] = 0.5f * (a.position.x + b.position.x);
        head.p[1] = 0.5f * (a.position.y + b.position.y);
        head.p[2] = 0.5f * (a.position.z + b.position.z);
        headOk = n > 1e-3f && std::isfinite(n) && std::isfinite(head.p[0] + head.p[1] + head.p[2]);
        if (headOk) for (float& c : head.q) c /= n;
    }
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* h = info->layers[i];
        if (!h || i == projIdx) continue;
        const XrStructureType t = h->type;
        const bool behind = i < projIdx;
        OverlayLayer L;
        switch (t) {
        case XR_TYPE_COMPOSITION_LAYER_QUAD: L.kind = OverlayKind::Quad; break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: L.kind = OverlayKind::Cylinder; break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: L.kind = OverlayKind::Equirect; break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: L.kind = OverlayKind::Equirect2; break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: L.kind = OverlayKind::Cube; break;
        default:
            snprintf(sig, sizeof(sig), "type %d", (int)t);
            if (t == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                OvNote(sd, sig, "layer %u is a second projection layer -- not recorded (only the first projection layer is captured)", i + 1);
            else
                OvNote(sd, sig, "layer %u (%s, XrStructureType %d)%s -- not recorded: this layer type is not supported (no "
                       "swapchain image to copy, or unknown to this layer)", i + 1, LayerTypeName(t), (int)t,
                       behind ? " before the projection layer" : "");
            continue;
        }
        L.behind = behind;
        const char* kind = OverlayKindName(L.kind);
        if (behind && !projBlend) {
            snprintf(sig, sizeof(sig), "below %d %p", (int)t, (void*)h->space);
            OvNote(sd, sig, "layer %u (%s) is BEFORE the projection layer, which is opaque (no BLEND_TEXTURE_SOURCE_ALPHA) -- "
                   "hidden behind the 3D view, not recorded", i + 1, kind);
            continue;
        }
        // the layer's image, eye visibility and pose
        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrRect2Di rect{};
        uint32_t slice = 0;
        XrEyeVisibility vis = XR_EYE_VISIBILITY_BOTH;
        XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
        bool geoOk = true;
        auto finite = [](float v) { return std::isfinite(v); };
        auto radiusOk = [](float r) { return r >= 0 && !std::isnan(r); };   // 0 or +inf = infinite sphere
        switch (L.kind) {
        case OverlayKind::Quad: {
            const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(h);
            swapchain = q->subImage.swapchain; rect = q->subImage.imageRect; slice = q->subImage.imageArrayIndex;
            vis = q->eyeVisibility; pose = q->pose;
            L.width = q->size.width; L.height = q->size.height;
            geoOk = L.width > 0 && L.height > 0 && finite(L.width) && finite(L.height);
            break;
        }
        case OverlayKind::Cylinder: {
            const auto* c = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(h);
            swapchain = c->subImage.swapchain; rect = c->subImage.imageRect; slice = c->subImage.imageArrayIndex;
            vis = c->eyeVisibility; pose = c->pose;
            L.radius = c->radius; L.centralAngle = c->centralAngle; L.aspect = c->aspectRatio;
            geoOk = L.radius > 0 && L.centralAngle > 0 && L.aspect > 0 && finite(L.radius) && finite(L.centralAngle) && finite(L.aspect);
            break;
        }
        case OverlayKind::Equirect: {
            const auto* e = reinterpret_cast<const XrCompositionLayerEquirectKHR*>(h);
            swapchain = e->subImage.swapchain; rect = e->subImage.imageRect; slice = e->subImage.imageArrayIndex;
            vis = e->eyeVisibility; pose = e->pose;
            L.radius = e->radius;
            L.eqScale[0] = e->scale.x; L.eqScale[1] = e->scale.y; L.eqBias[0] = e->bias.x; L.eqBias[1] = e->bias.y;
            geoOk = radiusOk(L.radius) && finite(L.eqScale[0]) && finite(L.eqScale[1]) && finite(L.eqBias[0]) && finite(L.eqBias[1]);
            break;
        }
        case OverlayKind::Equirect2: {
            const auto* e = reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(h);
            swapchain = e->subImage.swapchain; rect = e->subImage.imageRect; slice = e->subImage.imageArrayIndex;
            vis = e->eyeVisibility; pose = e->pose;
            L.radius = e->radius;
            L.centralHorizontalAngle = e->centralHorizontalAngle;
            L.upperVerticalAngle = e->upperVerticalAngle;
            L.lowerVerticalAngle = e->lowerVerticalAngle;
            geoOk = radiusOk(L.radius) && L.centralHorizontalAngle > 0 && L.centralHorizontalAngle <= 6.2832f &&
                    L.upperVerticalAngle > L.lowerVerticalAngle && L.upperVerticalAngle <= 1.5708f && L.lowerVerticalAngle >= -1.5708f;
            break;
        }
        case OverlayKind::Cube: {
            const auto* c = reinterpret_cast<const XrCompositionLayerCubeKHR*>(h);
            swapchain = c->swapchain; slice = c->imageArrayIndex;
            vis = c->eyeVisibility; pose.orientation = c->orientation;
            break;
        }
        }
        SwapchainData* scd = FindSwapchain(swapchain);
        snprintf(sig, sizeof(sig), "%s %p %p%s", kind, (void*)swapchain, (void*)h->space, behind ? " behind" : "");
        auto skip = [&](const char* why) {
            std::string s = std::string(sig) + " " + why;
            OvNote(sd, s, "layer %u (%s, swapchain %p, space %p) -- not recorded: %s", i + 1, kind, (void*)swapchain,
                   (void*)h->space, why);
        };
        if (!scd || !scd->info.format) { skip("its swapchain was not created through this layer"); continue; }
        if (scd->lastReleased < 0 || (size_t)scd->lastReleased >= scd->images.size()) { skip("no released swapchain image yet"); continue; }
        if (scd->info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) { skip("depth swapchain"); continue; }
        L.family = OverlayFamily((DXGI_FORMAT)scd->info.format);
        if (L.family < 0) {
            char why[120];
            snprintf(why, sizeof(why), "swapchain format %s not supported", FormatName(sd->api, scd->info.format).c_str());
            skip(why);
            continue;
        }
        int32_t x = 0, y = 0, w = 0, hh = 0;
        if (L.kind == OverlayKind::Cube) {
            if (scd->info.faceCount != 6) { skip("cube layer on a swapchain with faceCount != 6"); continue; }
            if (scd->info.sampleCount > 1) { skip("multisampled cube swapchain"); continue; }
            if (scd->info.width != scd->info.height || slice >= scd->info.arraySize) { skip("cube faces not square / bad array index"); continue; }
            w = (int32_t)scd->info.width; hh = (int32_t)scd->info.height;
            L.arrayIndex = slice * 6;                // face f = array slice imageArrayIndex * 6 + f
        } else {
            x = rect.offset.x; y = rect.offset.y; w = rect.extent.width; hh = rect.extent.height;
            if (x < 0) { w += x; x = 0; }
            if (y < 0) { hh += y; y = 0; }
            if (x + w > (int32_t)scd->info.width) w = (int32_t)scd->info.width - x;
            if (y + hh > (int32_t)scd->info.height) hh = (int32_t)scd->info.height - y;
            if (w <= 0 || hh <= 0 || slice >= scd->info.arraySize * (scd->info.faceCount ? scd->info.faceCount : 1)) {
                skip("empty sub-image rectangle / bad array index");
                continue;
            }
            L.arrayIndex = slice;
        }
        if (!geoOk) { skip("bad size / radius / angles"); continue; }
        // the layer's space -> the projection layer's space
        float sq[4] = {0, 0, 0, 1}, sp[3] = {0, 0, 0};
        const char* spaceHow = "same space as the projection layer";
        const auto vsp = h->space != proj->space ? g_viewSpaces.find(h->space) : g_viewSpaces.end();
        if (vsp != g_viewSpaces.end() && vsp->second.session == sd->handle && headOk) {
            // VIEW space (head-locked): placed on the head of the SUBMITTED views, not on a located head -- the runtime shows
            // it fixed to the display, and the recorded picture is the one rendered from the submitted poses, so the layer
            // stays at a fixed place in the video even when the game submits poses that differ from xrLocateSpace's
            const float* vq = vsp->second.q;
            QuatMul(head.q, vq, sq);
            QuatRotate(head.q, vsp->second.p, sp);
            sp[0] += head.p[0]; sp[1] += head.p[1]; sp[2] += head.p[2];
            // head smoothing turns the recorded camera away from this head: the recorder moves the layer with it
            // (OverlayHeadLock), so it stays fixed in the video like a HUD instead of shaking with the residual jitter
            L.headLocked = true;
            spaceHow = "VIEW space: on the head of the submitted views, head-locked";
        } else if (h->space != proj->space) {   // any other space: located at the frame's display time (cached per space)
            Located* lc = nullptr;
            for (uint32_t k = 0; k < nCache; ++k) if (cache[k].space == h->space) lc = &cache[k];
            if (!lc && nCache < kMaxOverlays) {
                lc = &cache[nCache++];
                lc->space = h->space;
                lc->ok = false;
                lc->r = XR_ERROR_FUNCTION_UNSUPPORTED;
                lc->flags = 0;
                if (sd->inst->LocateSpace) {
                    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
                    lc->r = sd->inst->LocateSpace(h->space, proj->space, info->displayTime, &loc);
                    lc->flags = loc.locationFlags;
                    lc->ok = XR_SUCCEEDED(lc->r) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                             (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
                    lc->pose = loc.pose;
                }
            }
            if (!lc || !lc->ok) {
                char why[120];
                snprintf(why, sizeof(why), "its space could not be located in the projection space (xrLocateSpace %d, flags 0x%llx)",
                         lc ? (int)lc->r : 0, lc ? (unsigned long long)lc->flags : 0ull);
                skip(why);
                continue;
            }
            sq[0] = lc->pose.orientation.x; sq[1] = lc->pose.orientation.y; sq[2] = lc->pose.orientation.z; sq[3] = lc->pose.orientation.w;
            sp[0] = lc->pose.position.x; sp[1] = lc->pose.position.y; sp[2] = lc->pose.position.z;
            spaceHow = "located in the projection space at displayTime";
        }
        const float lq[4] = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
        const float lp[3] = {pose.position.x, pose.position.y, pose.position.z};
        QuatMul(sq, lq, L.q);
        QuatRotate(sq, lp, L.p);
        L.p[0] += sp[0]; L.p[1] += sp[1]; L.p[2] += sp[2];
        const float qn = std::sqrt(L.q[0] * L.q[0] + L.q[1] * L.q[1] + L.q[2] * L.q[2] + L.q[3] * L.q[3]);
        if (!(qn > 0.5f && qn < 1.5f) || !std::isfinite(L.p[0] + L.p[1] + L.p[2])) { skip("bad orientation / position"); continue; }
        for (float& c : L.q) c /= qn;
        // eye visibility -> output eyes (swap_eyes: output eye 0 shows the right eye)
        const uint8_t left = swapped ? 2 : 1, right = swapped ? 1 : 2;
        L.eyeMask = vis == XR_EYE_VISIBILITY_LEFT ? left : vis == XR_EYE_VISIBILITY_RIGHT ? right : 3;
        L.alpha = (h->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
        L.unpremultiplied = (h->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
        L.srgb = IsSrgbDxgi((DXGI_FORMAT)scd->info.format);
        const XrCompositionLayerColorScaleBiasKHR* csb = FindColorScaleBias(h->next);
        if (csb) {
            const float v[8] = {csb->colorScale.r, csb->colorScale.g, csb->colorScale.b, csb->colorScale.a,
                                csb->colorBias.r, csb->colorBias.g, csb->colorBias.b, csb->colorBias.a};
            bool ok = true;
            for (float c : v) ok = ok && std::isfinite(c);
            if (!ok) { skip("non-finite XrCompositionLayerColorScaleBiasKHR"); continue; }
            for (int k = 0; k < 4; ++k) { L.colorScale[k] = v[k]; L.colorBias[k] = v[4 + k]; }
        }
        L.image = scd->images[(size_t)scd->lastReleased];
        L.sc = &scd->info;
        L.scKey = scd;
        L.x = x; L.y = y; L.w = (uint32_t)w; L.h = (uint32_t)hh;
        if (out.n >= kMaxOverlays) { skip("more than 16 overlay layers in one frame"); continue; }
        out.layer[out.n++] = L;
        if (behind) ++out.nBehind;
        {
            std::string s = std::string(sig) + " recorded";
            char geo[200] = "";
            switch (L.kind) {
            case OverlayKind::Quad: snprintf(geo, sizeof(geo), "size %.3f x %.3f m", L.width, L.height); break;
            case OverlayKind::Cylinder:
                snprintf(geo, sizeof(geo), "radius %.3f m, angle %.1f deg, aspect %.2f", L.radius, L.centralAngle * 57.29578f, L.aspect);
                break;
            case OverlayKind::Equirect:
                snprintf(geo, sizeof(geo), "radius %.3f m%s, scale (%.3f, %.3f), bias (%.3f, %.3f)", L.radius,
                         (L.radius == 0 || std::isinf(L.radius)) ? " (infinite)" : "", L.eqScale[0], L.eqScale[1], L.eqBias[0], L.eqBias[1]);
                break;
            case OverlayKind::Equirect2:
                snprintf(geo, sizeof(geo), "radius %.3f m%s, horizontal %.1f deg, vertical %+.1f .. %+.1f deg", L.radius,
                         (L.radius == 0 || std::isinf(L.radius)) ? " (infinite)" : "", L.centralHorizontalAngle * 57.29578f,
                         L.lowerVerticalAngle * 57.29578f, L.upperVerticalAngle * 57.29578f);
                break;
            case OverlayKind::Cube: snprintf(geo, sizeof(geo), "cube faces %ux%u (slices %u..%u)", w, hh, L.arrayIndex, L.arrayIndex + 5); break;
            }
            char extra[160] = "";
            if (csb)
                snprintf(extra, sizeof(extra), ", colour scale (%.2f %.2f %.2f %.2f) bias (%.2f %.2f %.2f %.2f)", L.colorScale[0],
                         L.colorScale[1], L.colorScale[2], L.colorScale[3], L.colorBias[0], L.colorBias[1], L.colorBias[2], L.colorBias[3]);
            char ms[40] = "";
            if (scd->info.sampleCount > 1) snprintf(ms, sizeof(ms), " %ux MSAA (resolved)", scd->info.sampleCount);
            OvNote(sd, s, "layer %u (%s) RECORDED%s: swapchain %p %s %ux%u%s, sub-image %d,%d %ux%u slice %u, %s, pose (%.2f, %.2f, %.2f) "
                   "in the projection space (%s), eyes %s, flags 0x%llx (%s)%s", i + 1, kind,
                   behind ? " BEHIND the projection layer (shows through its transparent pixels)" : "", (void*)swapchain,
                   FormatName(sd->api, scd->info.format).c_str(), scd->info.width, scd->info.height, ms,
                   x, y, w, hh, L.arrayIndex, geo, L.p[0], L.p[1], L.p[2], spaceHow,
                   vis == XR_EYE_VISIBILITY_LEFT ? "left" : vis == XR_EYE_VISIBILITY_RIGHT ? "right" : "both",
                   (unsigned long long)h->layerFlags,
                   !L.alpha ? "opaque" : L.unpremultiplied ? "alpha, unpremultiplied" : "alpha, premultiplied", extra);
        }
    }
}

void CaptureImpl(SessionData* sd, const XrFrameEndInfo* info) {
    ++sd->frames;
    sd->period.Add((int64_t)info->displayTime);

    bool wantStart = false;
    if (sd == g_active) wantStart = g_startRequested.exchange(false);

    const XrCompositionLayerProjection* proj = nullptr;
    uint32_t projIdx = 0;
    for (uint32_t i = 0; info->layers && i < info->layerCount; ++i) {
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
            projIdx = i;
            break;
        }
    }
    if (g_autoRecordSec > 0 && !sd->autoDone && proj && sd == g_active) {
        sd->autoDone = true;
        wantStart = true;
        sd->autoStopAt = QpcNow100ns() + (int64_t)(g_autoRecordSec * 1e7);
    }
    if (sd->autoStopAt && QpcNow100ns() >= sd->autoStopAt) {
        sd->autoStopAt = 0;
        LOGI("record: autorecord time reached -- stopping");
        sd->rec->Stop();
        if (g_autoRecord2Sec > 0 && !sd->auto2Done) { sd->auto2Done = true; sd->auto2At = QpcNow100ns() + 10000000; }   // second recording 1 s later
    }
    if (sd->auto2At && QpcNow100ns() >= sd->auto2At && sd->rec->State() == RecState::Idle && sd == g_active) {
        sd->auto2At = 0;
        wantStart = true;
        sd->autoStopAt = QpcNow100ns() + (int64_t)(g_autoRecord2Sec * 1e7);
        LOGI("XR_EYE_CAPTURE_AUTORECORD2=%.1f s: second recording", g_autoRecord2Sec);
    }
    if (!proj) {
        if (wantStart) g_startRequested = true;   // keep waiting (loading screens submit no projection layer)
        if (!sd->loggedNoProjection && sd->frames > 1) {
            sd->loggedNoProjection = true;
            LOGI("frame %llu has no projection layer (%u layers) -- nothing to capture on such frames",
                 (unsigned long long)sd->frames, info->layerCount);
        }
        return;
    }
    if (proj->viewCount < 2 || !proj->views) {
        if (!sd->loggedViewCount) { sd->loggedViewCount = true; LOGW("projection layer with viewCount %u -- need 2 views, not captured", proj->viewCount); }
        if (wantStart) g_startRequested = true;
        return;
    }

    EyeView eyes[2];
    for (int v = 0; v < 2; ++v) {
        const XrSwapchainSubImage& si = proj->views[v].subImage;
        SwapchainData* scd = FindSwapchain(si.swapchain);
        if (!scd || scd->lastReleased < 0 || (size_t)scd->lastReleased >= scd->images.size()) {
            if (!sd->loggedNoImage) {
                sd->loggedNoImage = true;
                LOGW("view %d: swapchain %p unknown or no released image yet (tracked %d, released %d) -- frame skipped", v,
                     (void*)si.swapchain, scd ? 1 : 0, scd ? scd->lastReleased : -1);
            }
            if (wantStart) g_startRequested = true;
            return;
        }
        EyeView& e = eyes[v];
        e.image = scd->images[(size_t)scd->lastReleased];
        e.sc = &scd->info;
        e.swapchainKey = scd;
        e.arrayIndex = si.imageArrayIndex;
        int32_t x = si.imageRect.offset.x, y = si.imageRect.offset.y;
        int32_t w = si.imageRect.extent.width, h = si.imageRect.extent.height;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > (int32_t)scd->info.width) w = (int32_t)scd->info.width - x;
        if (y + h > (int32_t)scd->info.height) h = (int32_t)scd->info.height - y;
        if (w <= 0 || h <= 0 || e.arrayIndex >= scd->info.arraySize) {
            if (wantStart) g_startRequested = true;
            return;
        }
        e.x = x; e.y = y; e.w = (uint32_t)w; e.h = (uint32_t)h;
        // rendering pose of this view (head-motion smoothing); in proj->space, which is the same for both views
        const XrCompositionLayerProjectionView& pv = proj->views[v];
        e.hasPose = true;
        e.orientation[0] = pv.pose.orientation.x; e.orientation[1] = pv.pose.orientation.y;
        e.orientation[2] = pv.pose.orientation.z; e.orientation[3] = pv.pose.orientation.w;
        e.position[0] = pv.pose.position.x; e.position[1] = pv.pose.position.y; e.position[2] = pv.pose.position.z;
        e.fov[0] = pv.fov.angleLeft; e.fov[1] = pv.fov.angleRight; e.fov[2] = pv.fov.angleUp; e.fov[3] = pv.fov.angleDown;
    }
    {
        // a recording is about to start: pick up config.ini edits (Settings window) before swap_eyes / dome are used
        const RecState rs0 = sd->rec->State();
        if (wantStart && rs0 != RecState::Starting && rs0 != RecState::Recording) ReloadConfigForRecording();
    }
    if (g_cfg.swapEyes) std::swap(eyes[0], eyes[1]);   // swap_eyes = 1: right eye goes into the left half
    LogLayout(sd, info, proj, eyes[0], eyes[1]);

    const RecState rs = sd->rec->State();
    if (wantStart && rs != RecState::Starting && rs != RecState::Recording) StartRecording(sd, eyes[0], eyes[1]);
    // record_overlays: only frames with more than the projection layer, while recording or while the live view composites
    // overlays (LiveShare::WantsOverlays), do any overlay work; one collection serves both
    const bool recording = sd->rec->State() == RecState::Recording;
    OverlayFrame ovf;
    const OverlayFrame* ovp = nullptr;
    if (info->layerCount > 1 && (recording || (sd->live && sd->live->WantsOverlays()))) {
        if (g_cfg.recordOverlays) {
            CollectOverlays(sd, info, projIdx, proj, g_cfg.swapEyes, ovf);
            if (ovf.n) ovp = &ovf;
        } else if (!sd->loggedOvOff) {
            sd->loggedOvOff = true;
            LOGI("overlay: record_overlays = 0 -- the %u other layer(s) of this frame (panels / overlays) are not recorded",
                 info->layerCount - 1);
        }
    }
    if (recording) sd->rec->SubmitFrame(VideoClock(sd, info->displayTime), eyes[0], eyes[1], (int64_t)info->displayTime, ovp);
    // live_share: the same views to a live viewer (OBS plugin), re-projected like a recording (g_cfg.head, with the overlay
    // layers when record_overlays = 1) unless the viewer asks for the raw eyes. Without a consumer this is a once-per-second
    // probe.
    if (sd->live) {
        DXGI_FORMAT stage = DXGI_FORMAT_UNKNOWN, stageR = DXGI_FORMAT_UNKNOWN;
        bool srgb = true, srgbR = true;
        if (!sd->src->StageFormat(*eyes[0].sc, &stage, &srgb) || !sd->src->StageFormat(*eyes[1].sc, &stageR, &srgbR) || stage != stageR)
            stage = DXGI_FORMAT_UNKNOWN;   // not shareable (logged by LiveShare when a consumer appears)
        if (g_cfg.gamma == GammaMode::Srgb) srgb = true;
        else if (g_cfg.gamma == GammaMode::Linear) srgb = false;
        sd->live->Frame(eyes[0], eyes[1], stage, srgb, (int64_t)(info->displayTime / 100), g_cfg.head, g_cfg.recordOverlays, ovp);
    }
}

// SEH wrapper: no C++ objects with destructors in this function.
bool CaptureSafe(SessionData* sd, const XrFrameEndInfo* info) {
    __try {
        CaptureImpl(sd, info);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ------------------------------------------------------------------------------------------------ indicator

IndicatorDispatch IndDispatch(const InstanceData* id) {
    IndicatorDispatch d;
    d.EnumerateSwapchainFormats = id->EnumerateSwapchainFormats;
    d.CreateSwapchain = id->CreateSwapchain;
    d.DestroySwapchain = id->DestroySwapchain;
    d.EnumerateSwapchainImages = id->EnumerateSwapchainImages;
    d.AcquireSwapchainImage = id->AcquireSwapchainImage;
    d.WaitSwapchainImage = id->WaitSwapchainImage;
    d.ReleaseSwapchainImage = id->ReleaseSwapchainImage;
    d.CreateReferenceSpace = id->CreateReferenceSpace;
    d.DestroySpace = id->DestroySpace;
    return d;
}

// Under g_lock, after the capture of this frame. true = `quad` is to be appended to this frame's layers.
bool IndicatorImpl(SessionData* sd, const XrFrameEndInfo* info, XrCompositionLayerQuad& quad) {
    const RecState st = sd->rec->State();
    std::string msg;
    if (sd->indTracker.NeedsMessage(st)) msg = sd->rec->Status().message;
    const double now = (double)QpcNow100ns() * 1e-7;
    // indicator_live: a live viewer receiving frames shows its own (steady violet) dot; recording wins
    const bool live = sd->indCfg.live && sd->live && sd->live->Active();
    const IndShow show = sd->indTracker.Update(st, msg, now, live);
    if (show == IndShow::Hidden) return false;                    // hidden: nothing at all is done
    if (info->layerCount + 1 > sd->maxLayers) {
        if (!sd->loggedIndLimit) {
            sd->loggedIndLimit = true;
            LOGW("indicator: the game already submits %u layers (runtime limit %u) -- indicator skipped on such frames",
                 info->layerCount, sd->maxLayers);
        }
        return false;
    }
    if (!sd->ind) {
        bool srgb = true;   // follow the game's colour swapchains (sRGB unless all of them are linear)
        bool any = false, anySrgb = false;
        for (auto& kv : g_swapchains) {
            const SwapchainData* s = kv.second.get();
            if (s->session != sd || !s->info.format || (s->info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) continue;
            any = true;
            if (IsSrgbDxgi((DXGI_FORMAT)s->info.format)) anySrgb = true;
        }
        if (any) srgb = anySrgb;
        sd->ind = std::make_unique<VrIndicator>(sd->indCfg, sd->api, sd->dev11.Get(), srgb);
    }
    if (sd->ind->Disabled()) { sd->indOff = true; return false; }
    if (!sd->ind->Frame(sd->handle, IndDispatch(sd->inst), show, now, quad)) {
        if (sd->ind->Disabled()) sd->indOff = true;
        return false;
    }
    if (!sd->loggedIndAppend) {
        sd->loggedIndAppend = true;
        LOGI("indicator: quad layer appended AFTER the capture on frame %llu (%u game layers + 1, state %s) -- shown in the "
             "headset only, never in the recording", (unsigned long long)sd->frames, info->layerCount, IndShowName(show));
    }
    return true;
}

// SEH wrapper: no C++ objects with destructors in this function.
bool IndicatorSafe(SessionData* sd, const XrFrameEndInfo* info, XrCompositionLayerQuad* quad, bool* fault) {
    __try {
        return IndicatorImpl(sd, info, *quad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return false;
    }
}

// Errors of xrEndFrame that our extra layer could have caused (then: retry without it, indicator off).
bool LayerError(XrResult r) {
    return r == XR_ERROR_LAYER_INVALID || r == XR_ERROR_LAYER_LIMIT_EXCEEDED || r == XR_ERROR_SWAPCHAIN_RECT_INVALID ||
           r == XR_ERROR_HANDLE_INVALID || r == XR_ERROR_VALIDATION_FAILURE || r == XR_ERROR_POSE_INVALID ||
           r == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED || r == XR_ERROR_RUNTIME_FAILURE;
}

// ------------------------------------------------------------------------------------------------ hooks

XrResult XRAPI_CALL Hook_EndFrame(XrSession session, const XrFrameEndInfo* info) {
    PFN_xrEndFrame next = nullptr;
    // Indicator: the game's layer pointers are forwarded untouched, plus our quad last. Both arrays live on this stack
    // frame for the whole call to the next layer.
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrFrameEndInfo withInd{};
    bool appended = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        SessionData* sd = FindSession(session);
        if (sd) {
            next = sd->inst->EndFrame;
            if (sd->src && sd->rec && !sd->faulted && info) {
                bool ok = false;
                try { ok = CaptureSafe(sd, info); } catch (...) { ok = false; }
                if (!ok) {
                    sd->faulted = true;
                    ++g_faults;
                    LOGE("capture faulted inside xrEndFrame -- capture disabled for this session, the game continues");
                    sd->rec->Stop();
                }
            }
            // after the capture: the recording has already taken this frame's projection views
            if (info && sd->armed && sd->rec && sd->indCfg.enabled && !sd->indOff) {
                bool fault = false;
                try { appended = IndicatorSafe(sd, info, &quad, &fault); } catch (...) { fault = true; appended = false; }
                if (fault) {
                    sd->indOff = true;
                    LOGE("indicator faulted inside xrEndFrame -- indicator off for this session (capture unaffected)");
                }
                if (appended) {
                    layers.reserve((size_t)info->layerCount + 1);
                    for (uint32_t i = 0; info->layers && i < info->layerCount; ++i) layers.push_back(info->layers[i]);
                    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
                    withInd = *info;
                    withInd.layerCount = (uint32_t)layers.size();
                    withInd.layers = layers.data();
                }
            }
        } else if (!g_instances.empty()) {
            next = g_instances.begin()->second->EndFrame;
        }
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    if (!appended) return next(session, info);
    const XrResult r = next(session, &withInd);
    if (XR_SUCCEEDED(r) || !LayerError(r)) return r;
    // The runtime rejected the frame with our quad: switch the indicator off and submit the game's frame as it was.
    LOGE("indicator: xrEndFrame with the indicator layer failed (%d) -- indicator off, resubmitting the game's layers", (int)r);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (SessionData* sd = FindSession(session)) {
            sd->indOff = true;
            if (sd->ind) sd->ind->Disable(IndDispatch(sd->inst), nullptr);
        }
    }
    const XrResult r2 = next(session, info);
    return XR_SUCCEEDED(r2) ? r2 : r;
}

XrResult XRAPI_CALL Hook_AcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* ai, uint32_t* index) {
    PFN_xrAcquireSwapchainImage next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        SwapchainData* scd = FindSwapchain(swapchain);
        if (scd) next = scd->session->inst->AcquireSwapchainImage;
        else if (!g_instances.empty()) next = g_instances.begin()->second->AcquireSwapchainImage;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = next(swapchain, ai, index);
    if (XR_SUCCEEDED(r) && index) {
        std::lock_guard<std::mutex> lk(g_lock);
        if (SwapchainData* scd = FindSwapchain(swapchain); scd && scd->session->armed) {
            scd->acquired.push_back(*index);
            if (scd->acquired.size() > 16) scd->acquired.pop_front();
        }
    }
    return r;
}

XrResult XRAPI_CALL Hook_ReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* ri) {
    PFN_xrReleaseSwapchainImage next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        SwapchainData* scd = FindSwapchain(swapchain);
        if (scd) next = scd->session->inst->ReleaseSwapchainImage;
        else if (!g_instances.empty()) next = g_instances.begin()->second->ReleaseSwapchainImage;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = next(swapchain, ri);
    if (XR_SUCCEEDED(r)) {
        std::lock_guard<std::mutex> lk(g_lock);
        if (SwapchainData* scd = FindSwapchain(swapchain); scd && scd->session->armed) {
            if (!scd->acquired.empty()) {
                scd->lastReleased = (int)scd->acquired.front();
                scd->acquired.pop_front();
            }
        }
    }
    return r;
}

XrResult XRAPI_CALL Hook_CreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* swapchain) {
    PFN_xrCreateSwapchain next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        SessionData* sd = FindSession(session);
        if (sd) next = sd->inst->CreateSwapchain;
        else if (!g_instances.empty()) next = g_instances.begin()->second->CreateSwapchain;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = next(session, ci, swapchain);
    if (XR_FAILED(r) || !ci || !swapchain) return r;
    try {
        std::lock_guard<std::mutex> lk(g_lock);
        SessionData* sd = FindSession(session);
        if (!sd) return r;
        auto scd = std::make_unique<SwapchainData>();
        scd->handle = *swapchain;
        scd->session = sd;
        if (!sd->armed) { g_swapchains[*swapchain] = std::move(scd); return r; }   // dormant: keep only the session link (dispatch)
        scd->info.format = ci->format;
        scd->info.width = ci->width;
        scd->info.height = ci->height;
        scd->info.arraySize = ci->arraySize ? ci->arraySize : 1;
        scd->info.mipCount = ci->mipCount ? ci->mipCount : 1;
        scd->info.sampleCount = ci->sampleCount ? ci->sampleCount : 1;
        scd->info.faceCount = ci->faceCount ? ci->faceCount : 1;
        scd->info.usageFlags = ci->usageFlags;
        LOGI("swapchain %p created: %s %ux%u array %u mips %u samples %u faces %u usage 0x%llx%s", (void*)*swapchain,
             FormatName(sd->api, ci->format).c_str(), ci->width, ci->height, ci->arraySize, ci->mipCount, ci->sampleCount,
             ci->faceCount, (unsigned long long)ci->usageFlags,
             (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? " (depth)" : "");
        g_swapchains[*swapchain] = std::move(scd);
    } catch (...) {
        LOGE("xrCreateSwapchain bookkeeping failed (ignored)");
    }
    return r;
}

XrResult XRAPI_CALL Hook_DestroySwapchain(XrSwapchain swapchain) {
    PFN_xrDestroySwapchain next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            SessionData* sd = it->second->session;
            next = sd->inst->DestroySwapchain;
            if (sd->src) sd->src->ForgetSwapchain(it->second.get());
            g_swapchains.erase(it);
        } else if (!g_instances.empty()) {
            next = g_instances.begin()->second->DestroySwapchain;
        }
    }
    return next ? next(swapchain) : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL Hook_CreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* ci, XrSpace* space) {
    PFN_xrCreateReferenceSpace next = nullptr;
    InstanceData* id = nullptr;
    bool armed = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        SessionData* sd = FindSession(session);
        if (sd) { id = sd->inst; armed = sd->armed; }
        else if (!g_instances.empty()) id = g_instances.begin()->second.get();
        if (id) next = id->CreateReferenceSpace;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = next(session, ci, space);
    if (XR_SUCCEEDED(r) && armed && ci && space && ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
        std::lock_guard<std::mutex> lk(g_lock);
        const XrPosef& p = ci->poseInReferenceSpace;
        g_viewSpaces[*space] = ViewSpaceData{session, id, {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
                                             {p.position.x, p.position.y, p.position.z}};
        LOGI("reference space %p created: VIEW (pose (%.3f, %.3f, %.3f)) -- overlay layers in it are placed on the submitted views' head",
             (void*)*space, p.position.x, p.position.y, p.position.z);
    }
    return r;
}

XrResult XRAPI_CALL Hook_DestroySpace(XrSpace space) {
    PFN_xrDestroySpace next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_viewSpaces.find(space);
        if (it != g_viewSpaces.end()) {
            if (it->second.inst) next = it->second.inst->DestroySpace;
            g_viewSpaces.erase(it);
        }
        if (!next && !g_instances.empty()) next = g_instances.begin()->second->DestroySpace;
    }
    return next ? next(space) : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL Hook_EnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count,
                                                  XrSwapchainImageBaseHeader* images) {
    PFN_xrEnumerateSwapchainImages next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        SwapchainData* scd = FindSwapchain(swapchain);
        if (scd) next = scd->session->inst->EnumerateSwapchainImages;
        else if (!g_instances.empty()) next = g_instances.begin()->second->EnumerateSwapchainImages;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = next(swapchain, capacity, count, images);
    if (XR_FAILED(r) || !images || capacity == 0 || !count) return r;
    std::lock_guard<std::mutex> lk(g_lock);
    SwapchainData* scd = FindSwapchain(swapchain);
    if (!scd || !scd->session->armed) return r;
    const uint32_t n = *count < capacity ? *count : capacity;
    scd->images.assign(n, nullptr);
    const XrStructureType t = images[0].type;
    for (uint32_t i = 0; i < n; ++i) {
        if (t == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) scd->images[i] = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[i].texture;
        else if (t == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) scd->images[i] = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[i].texture;
        else if (t == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
            // XrSwapchainImageVulkanKHR = {type, next, VkImage image}; VkImage is a 64-bit handle
            struct VkImg { XrStructureType type; void* next; uint64_t image; };
            scd->images[i] = (void*)(uintptr_t)reinterpret_cast<VkImg*>(images)[i].image;
        }
    }
    if (t == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR && n && scd->images[0]) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(scd->images[0])->GetDesc(&d);
        LOGI("swapchain %p: %u D3D11 images, texture %ux%u %s array %u samples %u bind 0x%x misc 0x%x%s", (void*)swapchain, n,
             d.Width, d.Height, DxgiFormatName(d.Format), d.ArraySize, d.SampleDesc.Count, d.BindFlags, d.MiscFlags,
             (d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) ? " (KEYED MUTEX: capture reads after release; watch for stale frames)" : "");
    } else if (t == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR && n && scd->images[0]) {
        const D3D12_RESOURCE_DESC d = static_cast<ID3D12Resource*>(scd->images[0])->GetDesc();
        LOGI("swapchain %p: %u D3D12 images, resource %llux%u %s array %u samples %u flags 0x%x", (void*)swapchain, n,
             (unsigned long long)d.Width, d.Height, DxgiFormatName(d.Format), d.DepthOrArraySize, d.SampleDesc.Count, (unsigned)d.Flags);
    } else {
        LOGI("swapchain %p: %u images (struct type %d)", (void*)swapchain, n, (int)t);
    }
    return r;
}

XrResult XRAPI_CALL Hook_CreateSession(XrInstance instance, const XrSessionCreateInfo* ci, XrSession* session) {
    PFN_xrCreateSession next = nullptr;
    InstanceData* id = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        id = FindInstance(instance);
        if (id) next = id->CreateSession;
    }
    if (!next) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = next(instance, ci, session);
    if (XR_FAILED(r) || !ci || !session) {
        LOGI("xrCreateSession returned %d (no capture)", (int)r);
        return r;
    }
    try {
        bool armedSession = false;
        auto sd = std::make_unique<SessionData>();
        sd->handle = *session;
        sd->inst = id;
        sd->armed = !g_cfg.requireTray || EyeCapTrayRunning();
        if (!sd->armed)
            LOGI("EyeCaptureTray not running — layer dormant for this session (set require_tray=0 in config.ini to always arm)");
        for (auto* p = reinterpret_cast<const XrBaseInStructure*>(ci->next); sd->armed && p; p = p->next) {
            if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                sd->api = GfxApi::D3D11;
                sd->dev11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(p)->device;
                sd->src = CreateD3D11FrameSource(reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(p)->device);
                break;
            }
            if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                sd->api = GfxApi::D3D12;
                auto* b = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(p);
                sd->src = CreateD3D12FrameSource(b->device, b->queue);
                break;
            }
            if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {   // == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR
                sd->api = GfxApi::Vulkan;
                sd->src = CreateVulkanFrameSource();
                LOGI("overlay: Vulkan session -- the capture path is a stub, so overlay layers (record_overlays) are not "
                     "recorded either");
                break;
            }
            if (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
                sd->api = GfxApi::OpenGL;
                LOGW("OpenGL session: unsupported graphics API, capture disabled (the game runs normally)");
                break;
            }
        }
        if (sd->armed) {
            if (sd->api == GfxApi::None) LOGW("session has no known graphics binding (headless?) -- capture disabled");
            if (sd->src) sd->rec = std::make_unique<Recorder>();
            if (sd->src && g_cfg.liveShare && (sd->api == GfxApi::D3D11 || sd->api == GfxApi::D3D12)) {
                LiveShareParams lp;
                lp.api = sd->api;
                for (auto* q = reinterpret_cast<const XrBaseInStructure*>(ci->next); q; q = q->next) {
                    if (q->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                        lp.dev11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(q)->device;
                        break;
                    }
                    if (q->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                        lp.dev12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(q)->device;
                        lp.queue12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(q)->queue;
                        break;
                    }
                }
                lp.game = g_exeName;
                lp.shmName = g_liveShm;
                sd->live = std::make_unique<LiveShare>(lp);
            }
            LOGI("session %p created: graphics API %s -> capture %s", (void*)*session, GfxApiName(sd->api),
                 sd->src ? "AVAILABLE" : "DISABLED");
            sd->indCfg = g_cfg.indicator;
            if (id->GetSystemProperties) {
                XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
                if (XR_SUCCEEDED(id->GetSystemProperties(instance, ci->systemId, &sp)) && sp.graphicsProperties.maxLayerCount)
                    sd->maxLayers = sp.graphicsProperties.maxLayerCount;
            }
            if (sd->rec && sd->indCfg.enabled)
                LOGI("indicator: on (vr_indicator = 1) -- dot in the headset while recording%s at (%.2f, %.2f, %.2f) m view "
                     "space, %.3f m, opacity %.2f%s", sd->indCfg.live && sd->live ? " and (violet) while a live viewer receives frames" : "",
                     sd->indCfg.x, sd->indCfg.y, sd->indCfg.z, sd->indCfg.size, sd->indCfg.opacity,
                     sd->api == GfxApi::D3D11 ? "" : " (D3D11 only for now)");
            else if (sd->rec)
                LOGI("indicator: off (vr_indicator = 0)");
        }
        armedSession = sd->armed;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            SessionData* raw = sd.get();
            g_sessions[*session] = std::move(sd);
            if (raw->src) g_active = raw;
        }
        if (armedSession) EnsureControl();
    } catch (...) {
        LOGE("xrCreateSession bookkeeping failed (capture disabled for this session)");
    }
    return r;
}

// After a session's recorder has finalized (or when it ends with a fresh "saved" result): hand the result to the tray
// right now. The control thread publishes only every 250 ms and is stopped with the last armed session, so a game that
// quits right after the recording ended would otherwise never deliver "saved <file>". Called without g_lock.
void PublishFinalStatus(SessionData& sd, bool finalizedHere) {
    RecorderStatus rs = sd.rec->Status();
    if (rs.message.empty()) return;
    if (!finalizedHere && rs.message.compare(0, 6, "saved ") != 0) return;
    ControlPublishFinal(rs, g_cfg.outputDir);
}

XrResult XRAPI_CALL Hook_DestroySession(XrSession session) {
    std::unique_ptr<SessionData> sd;
    PFN_xrDestroySession next = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_sessions.find(session);
        if (it != g_sessions.end()) {
            sd = std::move(it->second);
            g_sessions.erase(it);
            next = sd->inst->DestroySession;
            if (g_active == sd.get()) {
                g_active = nullptr;
                g_startRequested = false;
                for (auto& kv : g_sessions) if (kv.second->src) g_active = kv.second.get();
            }
            for (auto s = g_swapchains.begin(); s != g_swapchains.end();)
                s = (s->second->session == sd.get()) ? g_swapchains.erase(s) : std::next(s);
            for (auto v = g_viewSpaces.begin(); v != g_viewSpaces.end();)   // its spaces die with it
                v = v->second.session == session ? g_viewSpaces.erase(v) : std::next(v);
        } else if (!g_instances.empty()) {
            next = g_instances.begin()->second->DestroySession;
        }
    }
    if (sd) {
        if (sd->ind) {   // our swapchain / space go first, through the next layer, before the session is destroyed
            try { sd->ind->Destroy(IndDispatch(sd->inst)); } catch (...) { LOGE("indicator: cleanup failed (ignored)"); }
            sd->ind.reset();
        }
        if (sd->rec) {
            const RecState st = sd->rec->State();
            bool finalized = false;
            if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) {
                LOGI("session %p ending while recording -- finalizing the file", (void*)session);
                sd->rec->Stop();
                if (!sd->rec->WaitIdle(30000)) LOGE("finalize did not finish within 30 s");
                finalized = true;
            }
            if (sd->armed) PublishFinalStatus(*sd, finalized);
        }
        sd->live.reset();   // stops a live share (its teardown waits for the last copy on the game's GPU)
        LOGI("session %p destroyed after %llu frames%s", (void*)session, (unsigned long long)sd->frames,
             sd->armed ? "" : " (dormant)");
        const bool wasArmed = sd->armed;
        sd.reset();
        if (wasArmed) StopControlIfIdle();
    }
    return next ? next(session) : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL Hook_DestroyInstance(XrInstance instance) {
    std::unique_ptr<InstanceData> id;
    std::vector<std::unique_ptr<SessionData>> orphans;
    bool last = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) {
            id = std::move(it->second);
            g_instances.erase(it);
        }
        for (auto s = g_sessions.begin(); s != g_sessions.end();) {
            if (s->second->inst == id.get()) {
                if (g_active == s->second.get()) g_active = nullptr;
                orphans.push_back(std::move(s->second));
                s = g_sessions.erase(s);
            } else {
                ++s;
            }
        }
        for (auto v = g_viewSpaces.begin(); v != g_viewSpaces.end();)
            v = v->second.inst == id.get() ? g_viewSpaces.erase(v) : std::next(v);
        last = g_instances.empty();
    }
    for (auto& sd : orphans) {
        LOGW("instance destroyed with session %p still alive -- finalizing", (void*)sd->handle);
        if (sd->rec) {
            sd->rec->Stop();
            sd->rec->WaitIdle(30000);
            if (sd->armed) PublishFinalStatus(*sd, true);
        }
    }
    orphans.clear();
    if (last) StopControlIfIdle();
    PFN_xrDestroyInstance next = id ? id->DestroyInstance : nullptr;
    LOGI("instance destroyed");
    return next ? next(instance) : XR_ERROR_HANDLE_INVALID;
}

// ------------------------------------------------------------------------------------------------ control hooks

CaptureSnapshot Snapshot() {
    CaptureSnapshot s;
    std::lock_guard<std::mutex> lk(g_lock);
    s.game = g_exeName;
    s.outputDir = g_cfg.outputDir;
    if (g_active && g_active->src && g_active->rec) {
        s.capable = true;
        s.rec = g_active->rec->Status();
        if (g_startRequested.load() && s.rec.state == RecState::Failed) s.rec.state = RecState::Idle;   // a new start is pending
        s.api = elog::Widen(GfxApiName(g_active->api));
        s.runtime = elog::Widen(g_active->inst->runtimeName);
    }
    return s;
}

void RequestStart() { g_startRequested = true; }

void RequestStop() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_startRequested = false;
    if (g_active && g_active->rec) g_active->rec->Stop();
}

// ------------------------------------------------------------------------------------------------ dispatch

XrResult XRAPI_CALL Layer_GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

#define HOOK(fn) { "xr" #fn, (PFN_xrVoidFunction)Hook_##fn }
struct HookEntry { const char* name; PFN_xrVoidFunction fn; };
const HookEntry kHooks[] = {
    HOOK(DestroyInstance), HOOK(CreateSession), HOOK(DestroySession), HOOK(CreateSwapchain), HOOK(DestroySwapchain),
    HOOK(EnumerateSwapchainImages), HOOK(AcquireSwapchainImage), HOOK(ReleaseSwapchainImage), HOOK(EndFrame),
    HOOK(CreateReferenceSpace), HOOK(DestroySpace),
};

XrResult XRAPI_CALL Layer_GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = (PFN_xrVoidFunction)Layer_GetInstanceProcAddr;
        return XR_SUCCESS;
    }
    PFN_xrGetInstanceProcAddr next = nullptr;
    bool known = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        InstanceData* id = FindInstance(instance);
        known = id != nullptr;
        next = id ? id->nextGipa : g_anyNextGipa;
    }
    if (!next) { *function = nullptr; return XR_ERROR_HANDLE_INVALID; }
    const XrResult r = next(instance, name, function);
    if (XR_FAILED(r) || !*function || !known) return r;
    for (const HookEntry& h : kHooks)
        if (strcmp(name, h.name) == 0) { *function = h.fn; break; }
    return r;
}

void EnsureControl() {
    if (!g_trayIpc) return;
    std::lock_guard<std::mutex> lk(g_ctlLock);
    ControlStart(g_cfg, g_logPath, g_cfg.loadedFrom.empty() ? ConfigPath(g_dllDir) : g_cfg.loadedFrom,
                 ControlHooks{Snapshot, RequestStart, RequestStop});   // no-op if already running
}

void StopControlIfIdle() {
    std::lock_guard<std::mutex> ctl(g_ctlLock);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto& kv : g_sessions) if (kv.second->armed) return;
    }
    ControlStop();
}

XrResult XRAPI_CALL Layer_CreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* li, XrInstance* instance) {
    EnsureInit();
    if (!li || li->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO || !li->nextInfo ||
        li->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO || !li->nextInfo->nextCreateApiLayerInstance ||
        !li->nextInfo->nextGetInstanceProcAddr) {
        LOGE("xrCreateApiLayerInstance: invalid loader structures");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (strcmp(li->nextInfo->layerName, LAYER_NAME) != 0) {
        LOGE("xrCreateApiLayerInstance: chain names '%s', expected " LAYER_NAME, li->nextInfo->layerName);
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    XrApiLayerCreateInfo nextLi = *li;
    nextLi.nextInfo = li->nextInfo->next;
    const XrResult r = li->nextInfo->nextCreateApiLayerInstance(info, &nextLi, instance);
    if (XR_FAILED(r)) {
        LOGI("xrCreateInstance (next in chain) returned %d -- layer idle", (int)r);
        return r;
    }
    try {
        auto id = std::make_unique<InstanceData>();
        id->handle = *instance;
        id->nextGipa = li->nextInfo->nextGetInstanceProcAddr;
#define RESOLVE(fn) id->nextGipa(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction*>(&id->fn))
        RESOLVE(DestroyInstance); RESOLVE(CreateSession); RESOLVE(DestroySession); RESOLVE(CreateSwapchain);
        RESOLVE(DestroySwapchain); RESOLVE(EnumerateSwapchainImages); RESOLVE(AcquireSwapchainImage);
        RESOLVE(ReleaseSwapchainImage); RESOLVE(EndFrame); RESOLVE(GetInstanceProperties);
        RESOLVE(EnumerateSwapchainFormats); RESOLVE(WaitSwapchainImage); RESOLVE(CreateReferenceSpace); RESOLVE(DestroySpace);
        RESOLVE(GetSystemProperties); RESOLVE(LocateSpace);
#undef RESOLVE
        std::string exts;
        bool fb = false;
        for (uint32_t i = 0; info && i < info->enabledExtensionCount; ++i) {
            exts += info->enabledExtensionNames[i];
            exts += ' ';
            if (strcmp(info->enabledExtensionNames[i], "XR_FB_display_refresh_rate") == 0) fb = true;
        }
        if (fb) id->nextGipa(*instance, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&id->GetDisplayRefreshRateFB));
        if (id->GetInstanceProperties) {
            XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
            if (XR_SUCCEEDED(id->GetInstanceProperties(*instance, &ip)))
                id->runtimeName = std::string(ip.runtimeName) + " " + std::to_string(XR_VERSION_MAJOR(ip.runtimeVersion)) + "." +
                                  std::to_string(XR_VERSION_MINOR(ip.runtimeVersion)) + "." + std::to_string(XR_VERSION_PATCH(ip.runtimeVersion));
        }
        if (info) id->appName = std::string(info->applicationInfo.applicationName) + " / engine " + info->applicationInfo.engineName;
        LOGI("instance %p created: app '%s', runtime '%s', extensions: %s", (void*)*instance, id->appName.c_str(),
             id->runtimeName.c_str(), exts.c_str());
        if (!id->EndFrame || !id->CreateSession) LOGE("next layer/runtime did not return core functions -- capture will not work");
        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_anyNextGipa = id->nextGipa;
            g_instances[*instance] = std::move(id);
        }
    } catch (...) {
        LOGE("instance bookkeeping failed -- layer passes everything through");
    }
    return r;
}

} // namespace

// ------------------------------------------------------------------------------------------------ export

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                                         const char* layerName,
                                                                                         XrNegotiateApiLayerRequest* req) {
    EnsureInit();
    if (!loaderInfo || !req || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST || req->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        req->structSize != sizeof(XrNegotiateApiLayerRequest)) {
        LOGE("negotiate: unexpected loader structures");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (layerName && strcmp(layerName, LAYER_NAME) != 0) {
        LOGE("negotiate: asked for layer '%s'", layerName);
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION || loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        LOGE("negotiate: loader interface %u..%u unsupported", loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion);
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    XrVersion api = XR_CURRENT_API_VERSION;
    if (api > loaderInfo->maxApiVersion) api = loaderInfo->maxApiVersion;
    if (api < loaderInfo->minApiVersion) api = loaderInfo->minApiVersion;
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = api;
    req->getInstanceProcAddr = Layer_GetInstanceProcAddr;
    req->createApiLayerInstance = Layer_CreateApiLayerInstance;
    LOGI("negotiate: OK (loader API %u.%u..%u.%u, layer reports %u.%u)", XR_VERSION_MAJOR(loaderInfo->minApiVersion),
         XR_VERSION_MINOR(loaderInfo->minApiVersion), XR_VERSION_MAJOR(loaderInfo->maxApiVersion),
         XR_VERSION_MINOR(loaderInfo->maxApiVersion), XR_VERSION_MAJOR(api), XR_VERSION_MINOR(api));
    return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // nothing heavy here (loader lock); setup runs on the first negotiate call
    }
    return TRUE;
}
