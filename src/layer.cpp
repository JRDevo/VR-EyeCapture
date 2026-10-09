// layer -- XR_APILAYER_JRDEVO_eye_capture: an implicit OpenXR API layer that records both eye images of any OpenXR
// game into a side-by-side 3D MP4.
//
// Call chain: loader -> [this layer] -> other layers -> runtime. Everything not hooked is forwarded untouched (the
// layer's xrGetInstanceProcAddr returns the next layer's pointers). Hooked:
//   xrCreateSession        find the graphics binding (D3D11 / D3D12 / Vulkan / OpenGL) -> IFrameSource
//   xrDestroySession       stop + finalize the recording, release
//   xrCreateSwapchain / xrDestroySwapchain / xrEnumerateSwapchainImages   track images + format/size/array/samples
//   xrAcquireSwapchainImage / xrReleaseSwapchainImage                     track the most recently released image
//   xrEndFrame             first projection layer: views[0] = left, views[1] = right -> Recorder::SubmitFrame, then forward
//   xrDestroyInstance      clean up
// Rule: any internal failure logs and passes through. The game's calls are always forwarded and their results returned
// unchanged; capture work in xrEndFrame is wrapped in SEH so a fault disables capture instead of crashing the game.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <shlobj.h>
#include <atomic>
#include <cmath>
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

#include "config.h"
#include "control.h"
#include "frame_source.h"
#include "log.h"
#include "recorder.h"

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
    bool faulted = false;
    uint64_t frames = 0;
    // submitted-frame period estimate (displayTime deltas)
    XrTime lastDisplayTime = 0;
    double periodNs = 0;
    int periodSamples = 0;
    // clock anchor of the current recording
    bool anchored = false;
    int64_t anchorQpc = 0;
    XrTime anchorDisp = 0;
    int reanchors = 0;
    // layout logging
    std::string layoutSig;
    int layoutLogs = 0;
    bool loggedNoProjection = false, loggedViewCount = false, loggedNoImage = false;
    // autorecord (XR_EYE_CAPTURE_AUTORECORD=<seconds>, for tests / automation)
    bool autoDone = false;
    int64_t autoStopAt = 0;
};

std::mutex g_lock;
std::unordered_map<XrInstance, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<XrSession, std::unique_ptr<SessionData>> g_sessions;
std::unordered_map<XrSwapchain, std::unique_ptr<SwapchainData>> g_swapchains;
SessionData* g_active = nullptr;              // newest capture-capable session (hotkey / tray act on it)
std::atomic<bool> g_startRequested{false};
std::atomic<uint64_t> g_faults{0};
PFN_xrGetInstanceProcAddr g_anyNextGipa = nullptr;

// process-wide setup
std::once_flag g_initOnce;
Config g_cfg;
std::wstring g_dllDir, g_exeName, g_logPath;
double g_autoRecordSec = 0;

// ------------------------------------------------------------------------------------------------ helpers

std::wstring ModuleDir() {
    HMODULE hm = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ModuleDir, &hm);
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(hm, p, MAX_PATH);
    std::wstring s = p;
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

std::wstring ExeBaseName() {
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    const size_t k = s.find_last_of(L"\\/");
    if (k != std::wstring::npos) s = s.substr(k + 1);
    const size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos) s = s.substr(0, dot);
    return s.empty() ? L"game" : s;
}

std::wstring EnvW(const wchar_t* name) {
    wchar_t b[1024];
    const DWORD n = GetEnvironmentVariableW(name, b, 1024);
    return (n > 0 && n < 1024) ? std::wstring(b) : std::wstring();
}

void InitProcess() {
    g_dllDir = ModuleDir();
    g_exeName = ExeBaseName();
    g_logPath = g_dllDir + L"\\logs\\eye_capture.log";
    elog::Init(g_logPath, false);
    LoadConfig(g_dllDir, g_cfg);
    const std::wstring outOverride = EnvW(L"XR_EYE_CAPTURE_OUTPUT_DIR");
    if (!outOverride.empty()) g_cfg.outputDir = outOverride;
    const std::wstring autoRec = EnvW(L"XR_EYE_CAPTURE_AUTORECORD");
    if (!autoRec.empty()) g_autoRecordSec = _wtof(autoRec.c_str());
    elog::SetLevel(g_cfg.logLevel);
    elog::SetMirror(g_cfg.outputDir + L"\\eye_capture.log");
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LOGI("==== " LAYER_NAME " v%s loaded into %s (pid %lu)", EYE_CAPTURE_VERSION, elog::Narrow(exe).c_str(),
         (unsigned long)GetCurrentProcessId());
    LOGI("layer dll folder %s | config %s | output %s | hotkey %s | codec %s %u Mb/s | max_width %u | max_fps %u | "
         "audio %s%s%s | gamma %s",
         elog::Narrow(g_dllDir).c_str(), g_cfg.loadedFrom.empty() ? "(defaults)" : elog::Narrow(g_cfg.loadedFrom).c_str(),
         elog::Narrow(g_cfg.outputDir).c_str(), elog::Narrow(g_cfg.hotkey.text).c_str(), g_cfg.hevc ? "HEVC" : "H.264",
         g_cfg.bitrateMbps ? g_cfg.bitrateMbps : (g_cfg.hevc ? 120u : 80u), g_cfg.maxWidth, g_cfg.maxFps,
         g_cfg.audioEnabled ? "on" : "off", g_cfg.audioDevice.empty() ? "" : " device~",
         elog::Narrow(g_cfg.audioDevice).c_str(),
         g_cfg.gamma == GammaMode::Auto ? "auto" : g_cfg.gamma == GammaMode::Srgb ? "srgb" : "linear");
    if (g_autoRecordSec > 0) LOGI("XR_EYE_CAPTURE_AUTORECORD=%.1f s: recording starts at the first frame", g_autoRecordSec);
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

unsigned SnapRate(double hz) {
    const unsigned common[] = {45, 60, 72, 80, 90, 120, 144};
    for (unsigned c : common) if (std::fabs(hz - c) <= c * 0.03) return c;
    return (unsigned)std::lround(hz);
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
    unsigned fps = 0;
    const char* fpsFrom = "config";
    float refresh = 0;
    if (sd->inst->GetDisplayRefreshRateFB) sd->inst->GetDisplayRefreshRateFB(sd->handle, &refresh);
    if (sd->periodSamples >= 30 && sd->periodNs > 0) { fps = SnapRate(1e9 / sd->periodNs); fpsFrom = "measured frame submit rate"; }
    else if (refresh > 1) { fps = SnapRate(refresh); fpsFrom = "XR_FB_display_refresh_rate"; }
    if (!fps) fps = g_cfg.fps;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf_s(name, L"%s_%04u%02u%02u_%02u%02u%02u_SBS.mp4", g_exeName.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    SHCreateDirectoryExW(nullptr, g_cfg.outputDir.c_str(), nullptr);

    RecorderSettings rs;
    rs.path = g_cfg.outputDir + L"\\" + name;
    rs.hevc = g_cfg.hevc;
    rs.bitrate = g_cfg.bitrateMbps * 1000000u;
    rs.fps = fps;
    rs.maxFps = g_cfg.maxFps;
    rs.maxWidth = g_cfg.maxWidth;
    rs.audio = g_cfg.audioEnabled;
    rs.audioDevice = g_cfg.audioDevice;
    const UINT W = L.w + R.w, H = L.h > R.h ? L.h : R.h;
    LOGI("record: start %s | %s | %ux%u SBS (eyes %ux%u + %ux%u) | %u fps from %s (display refresh %.1f, measured %.2f) | %s",
         elog::Narrow(rs.path).c_str(), GfxApiName(sd->api), W, H, L.w, L.h, R.w, R.h, fps, fpsFrom, refresh,
         sd->periodNs > 0 ? 1e9 / sd->periodNs : 0.0, srgb ? "sRGB-encoded" : "linear");
    sd->anchored = false;
    sd->reanchors = 0;
    if (!sd->rec->Start(sd->src.get(), rs, W, H, L.w, stage, srgb))
        LOGW("record: previous recording still finalizing -- start ignored");
}

int64_t VideoClock(SessionData* sd, XrTime displayTime) {
    const int64_t now = QpcNow100ns();
    if (!sd->anchored) {
        sd->anchored = true;
        sd->anchorQpc = now;
        sd->anchorDisp = displayTime;
    }
    int64_t ts = sd->anchorQpc + (displayTime - sd->anchorDisp) / 100;
    if (ts - now > 5000000 || now - ts > 5000000) {   // runtime clock jumped (pause / resync): re-anchor on QPC
        if (++sd->reanchors <= 5) LOGW("record: displayTime drifted %.1f ms from QPC -- re-anchoring", (ts - now) / 1e4);
        sd->anchorQpc = now;
        sd->anchorDisp = displayTime;
        ts = now;
    }
    return ts;
}

void CaptureImpl(SessionData* sd, const XrFrameEndInfo* info) {
    ++sd->frames;
    if (sd->lastDisplayTime && info->displayTime > sd->lastDisplayTime) {
        const double d = (double)(info->displayTime - sd->lastDisplayTime);
        if (d > 2e6 && d < 1e8) {
            sd->periodNs = sd->periodSamples ? sd->periodNs * 0.95 + d * 0.05 : d;
            ++sd->periodSamples;
        }
    }
    sd->lastDisplayTime = info->displayTime;

    bool wantStart = false;
    if (sd == g_active) wantStart = g_startRequested.exchange(false);

    const XrCompositionLayerProjection* proj = nullptr;
    for (uint32_t i = 0; info->layers && i < info->layerCount; ++i) {
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            proj = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
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
    }
    LogLayout(sd, info, proj, eyes[0], eyes[1]);

    const RecState rs = sd->rec->State();
    if (wantStart && rs != RecState::Starting && rs != RecState::Recording) StartRecording(sd, eyes[0], eyes[1]);
    if (sd->rec->State() == RecState::Recording) sd->rec->SubmitFrame(VideoClock(sd, info->displayTime), eyes[0], eyes[1]);
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

// ------------------------------------------------------------------------------------------------ hooks

XrResult XRAPI_CALL Hook_EndFrame(XrSession session, const XrFrameEndInfo* info) {
    PFN_xrEndFrame next = nullptr;
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
        } else if (!g_instances.empty()) {
            next = g_instances.begin()->second->EndFrame;
        }
    }
    return next ? next(session, info) : XR_ERROR_HANDLE_INVALID;
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
        if (SwapchainData* scd = FindSwapchain(swapchain)) {
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
        if (SwapchainData* scd = FindSwapchain(swapchain)) {
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
    if (!scd) return r;
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
        auto sd = std::make_unique<SessionData>();
        sd->handle = *session;
        sd->inst = id;
        for (auto* p = reinterpret_cast<const XrBaseInStructure*>(ci->next); p; p = p->next) {
            if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                sd->api = GfxApi::D3D11;
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
                break;
            }
            if (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
                sd->api = GfxApi::OpenGL;
                LOGW("OpenGL session: unsupported graphics API, capture disabled (the game runs normally)");
                break;
            }
        }
        if (sd->api == GfxApi::None) LOGW("session has no known graphics binding (headless?) -- capture disabled");
        if (sd->src) sd->rec = std::make_unique<Recorder>();
        LOGI("session %p created: graphics API %s -> capture %s", (void*)*session, GfxApiName(sd->api),
             sd->src ? "AVAILABLE" : "DISABLED");
        std::lock_guard<std::mutex> lk(g_lock);
        SessionData* raw = sd.get();
        g_sessions[*session] = std::move(sd);
        if (raw->src) g_active = raw;
    } catch (...) {
        LOGE("xrCreateSession bookkeeping failed (capture disabled for this session)");
    }
    return r;
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
        } else if (!g_instances.empty()) {
            next = g_instances.begin()->second->DestroySession;
        }
    }
    if (sd) {
        if (sd->rec) {
            const RecState st = sd->rec->State();
            if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) {
                LOGI("session %p ending while recording -- finalizing the file", (void*)session);
                sd->rec->Stop();
                if (!sd->rec->WaitIdle(30000)) LOGE("finalize did not finish within 30 s");
            }
        }
        LOGI("session %p destroyed after %llu frames", (void*)session, (unsigned long long)sd->frames);
        sd.reset();
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
        last = g_instances.empty();
    }
    for (auto& sd : orphans) {
        LOGW("instance destroyed with session %p still alive -- finalizing", (void*)sd->handle);
        if (sd->rec) { sd->rec->Stop(); sd->rec->WaitIdle(30000); }
    }
    orphans.clear();
    if (last) ControlStop();
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
        ControlStart(g_cfg, g_logPath, g_cfg.loadedFrom.empty() ? ConfigPath(g_dllDir) : g_cfg.loadedFrom,
                     ControlHooks{Snapshot, RequestStart, RequestStop});
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
