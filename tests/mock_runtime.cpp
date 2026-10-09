// mock_runtime -- a minimal OpenXR runtime for testing the layer without a headset (selected with XR_RUNTIME_JSON).
// Supports XR_KHR_D3D11_enable, XR_KHR_D3D12_enable and XR_FB_display_refresh_rate; swapchains are real textures on the
// app's device (TYPELESS for 8-bit formats, like real runtimes), 3 images each; xrWaitFrame paces at 90 Hz;
// xrEndFrame only validates and counts. xrLocateViews reports an identity head pose with a +-0.8 rad fov, or with
// EYECAP_MOCK_MOTION=jitter the head motion of tests/test_motion.h (jitter + a deliberate turn) and an asymmetric fov.
// Not a conformant runtime -- just enough for tests/layer_e2e.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include "test_motion.h"

using Microsoft::WRL::ComPtr;

namespace {
std::mutex g_m;
std::atomic<uint64_t> g_nextHandle{0x1000};
const int64_t kPeriodNs = 11111111;   // 90 Hz

struct Session {
    bool d3d12 = false;
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D12Device> dev12;
    uint64_t frames = 0, projFrames = 0;
    int64_t nextDisplay = 0;
    int64_t motionOrigin = 0;   // first predictedDisplayTime: t = 0 of tests/test_motion.h
    LARGE_INTEGER nextWake{};
};
struct Swapchain {
    XrSession session;
    std::vector<ComPtr<ID3D11Texture2D>> t11;
    std::vector<ComPtr<ID3D12Resource>> t12;
    uint32_t next = 0;
};
std::map<uint64_t, Session> g_sessions;
std::map<uint64_t, Swapchain> g_swapchains;

int64_t NowNs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

LUID PickLuid() {
    ComPtr<IDXGIFactory1> f;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    LUID best{};
    SIZE_T mem = 0;
    for (UINT i = 0; f && SUCCEEDED(f->EnumAdapters1(i, &a)); ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > mem) { mem = d.DedicatedVideoMemory; best = d.AdapterLuid; }
    }
    return best;
}

DXGI_FORMAT Typeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return f;
    }
}

XrResult XRAPI_CALL EnumerateInstanceExtensionProperties(const char*, uint32_t cap, uint32_t* count, XrExtensionProperties* props) {
    static const char* names[] = {"XR_KHR_D3D11_enable", "XR_KHR_D3D12_enable", "XR_FB_display_refresh_rate"};
    *count = 3;
    if (!cap) return XR_SUCCESS;
    if (cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 3; ++i) { strcpy_s(props[i].extensionName, names[i]); props[i].extensionVersion = 1; }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo*, XrInstance* inst) { *inst = (XrInstance)g_nextHandle++; return XR_SUCCESS; }
XrResult XRAPI_CALL DestroyInstance(XrInstance) { return XR_SUCCESS; }
XrResult XRAPI_CALL GetInstanceProperties(XrInstance, XrInstanceProperties* p) {
    strcpy_s(p->runtimeName, "EyeCapture Mock Runtime");
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* id) { *id = 1; return XR_SUCCESS; }
XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p) {
    strcpy_s(p->systemName, "Mock HMD");
    p->graphicsProperties.maxSwapchainImageWidth = 16384;
    p->graphicsProperties.maxSwapchainImageHeight = 16384;
    p->graphicsProperties.maxLayerCount = 16;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetD3D11Req(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* r) {
    r->adapterLuid = PickLuid();
    r->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetD3D12Req(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* r) {
    r->adapterLuid = PickLuid();
    r->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetDisplayRefreshRateFB(XrSession, float* hz) { *hz = 90.0f; return XR_SUCCESS; }
XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* s) {
    Session ss;
    for (auto* p = (const XrBaseInStructure*)ci->next; p; p = p->next) {
        if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) ss.dev11 = ((const XrGraphicsBindingD3D11KHR*)p)->device;
        if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) { ss.dev12 = ((const XrGraphicsBindingD3D12KHR*)p)->device; ss.d3d12 = true; }
    }
    if (!ss.dev11 && !ss.dev12) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    const uint64_t h = g_nextHandle++;
    std::lock_guard<std::mutex> lk(g_m);
    g_sessions[h] = ss;
    *s = (XrSession)h;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySession(XrSession s) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_sessions.find((uint64_t)s);
    if (it == g_sessions.end()) return XR_ERROR_HANDLE_INVALID;
    printf("[mock runtime] session destroyed: %llu frames, %llu with a valid projection layer\n",
           (unsigned long long)it->second.frames, (unsigned long long)it->second.projFrames);
    g_sessions.erase(it);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL BeginSession(XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL EndSession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer*) { return XR_EVENT_UNAVAILABLE; }
XrResult XRAPI_CALL WaitFrame(XrSession s, const XrFrameWaitInfo*, XrFrameState* fs) {
    Session* ss;
    { std::lock_guard<std::mutex> lk(g_m); auto it = g_sessions.find((uint64_t)s); if (it == g_sessions.end()) return XR_ERROR_HANDLE_INVALID; ss = &it->second; }
    int64_t now = NowNs();
    if (!ss->nextDisplay) ss->nextDisplay = ss->motionOrigin = now + 2 * kPeriodNs;
    const int64_t wake = ss->nextDisplay - 2 * kPeriodNs;
    while ((now = NowNs()) < wake) { if (wake - now > 2000000) Sleep(1); }
    fs->predictedDisplayTime = ss->nextDisplay;
    fs->predictedDisplayPeriod = kPeriodNs;
    fs->shouldRender = XR_TRUE;
    ss->nextDisplay += kPeriodNs;
    if (ss->nextDisplay < now) ss->nextDisplay = now + kPeriodNs;   // fell behind: skip
    return XR_SUCCESS;
}
XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL LocateViews(XrSession s, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* count, XrView* views) {
    int64_t origin = 0;
    { std::lock_guard<std::mutex> lk(g_m); auto it = g_sessions.find((uint64_t)s); if (it == g_sessions.end()) return XR_ERROR_HANDLE_INVALID; origin = it->second.motionOrigin; }
    *count = 2;
    if (!cap) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    char motion[32] = "";
    GetEnvironmentVariableA("EYECAP_MOCK_MOTION", motion, sizeof(motion));
    const bool jitter = strcmp(motion, "jitter") == 0;
    const double t = origin ? (double)(li->displayTime - origin) * 1e-9 : 0.0;
    const test_motion::Quat q = jitter ? test_motion::Head(t) : test_motion::Quat{0, 0, 0, 1};
    float m[9];
    test_motion::Matrix(q, m);
    for (uint32_t e = 0; e < 2; ++e) {
        const float ipd = e == 0 ? -0.032f : 0.032f;
        views[e].pose.orientation = {(float)q.x, (float)q.y, (float)q.z, (float)q.w};
        views[e].pose.position = {m[0] * ipd, 1.6f + m[3] * ipd, m[6] * ipd};
        const float* f = e == 0 ? test_motion::kFovLeft : test_motion::kFovRight;
        if (jitter) views[e].fov = {f[0], f[1], f[2], f[3]};
        else views[e].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    }
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                         XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EndFrame(XrSession s, const XrFrameEndInfo* info) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_sessions.find((uint64_t)s);
    if (it == g_sessions.end()) return XR_ERROR_HANDLE_INVALID;
    ++it->second.frames;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        auto* p = (const XrCompositionLayerProjection*)info->layers[i];
        bool ok = p->viewCount == 2;
        for (uint32_t v = 0; ok && v < 2; ++v) ok = g_swapchains.count((uint64_t)p->views[v].subImage.swapchain) != 0;
        if (ok) ++it->second.projFrames;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* count, int64_t* fmts) {
    const int64_t list[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
    *count = 5;
    if (!cap) return XR_SUCCESS;
    if (cap < 5) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(fmts, list, sizeof(list));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateSwapchain(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    Session ss;
    { std::lock_guard<std::mutex> lk(g_m); auto it = g_sessions.find((uint64_t)s); if (it == g_sessions.end()) return XR_ERROR_HANDLE_INVALID; ss = it->second; }
    Swapchain sc;
    sc.session = s;
    const DXGI_FORMAT tf = Typeless((DXGI_FORMAT)ci->format);
    for (int i = 0; i < 3; ++i) {
        if (!ss.d3d12) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = ci->width; d.Height = ci->height; d.MipLevels = ci->mipCount; d.ArraySize = ci->arraySize; d.Format = tf;
            d.SampleDesc.Count = ci->sampleCount; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> t;
            if (FAILED(ss.dev11->CreateTexture2D(&d, nullptr, &t))) return XR_ERROR_RUNTIME_FAILURE;
            sc.t11.push_back(t);
        } else {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = ci->width; rd.Height = ci->height; rd.DepthOrArraySize = (UINT16)ci->arraySize; rd.MipLevels = (UINT16)ci->mipCount;
            rd.Format = tf; rd.SampleDesc.Count = ci->sampleCount; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ComPtr<ID3D12Resource> r;
            if (FAILED(ss.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&r))))
                return XR_ERROR_RUNTIME_FAILURE;
            sc.t12.push_back(r);
        }
    }
    const uint64_t h = g_nextHandle++;
    std::lock_guard<std::mutex> lk(g_m);
    g_swapchains[h] = sc;
    *out = (XrSwapchain)h;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySwapchain(XrSwapchain s) {
    std::lock_guard<std::mutex> lk(g_m);
    return g_swapchains.erase((uint64_t)s) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
XrResult XRAPI_CALL EnumerateSwapchainImages(XrSwapchain s, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* imgs) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_swapchains.find((uint64_t)s);
    if (it == g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    const bool d12 = !it->second.t12.empty();
    *count = 3;
    if (!cap) return XR_SUCCESS;
    if (cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < 3; ++i) {
        if (d12) ((XrSwapchainImageD3D12KHR*)imgs)[i].texture = it->second.t12[i].Get();
        else ((XrSwapchainImageD3D11KHR*)imgs)[i].texture = it->second.t11[i].Get();
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain s, const XrSwapchainImageAcquireInfo*, uint32_t* idx) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_swapchains.find((uint64_t)s);
    if (it == g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    *idx = it->second.next;
    it->second.next = (it->second.next + 1) % 3;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL WaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* sp) { *sp = (XrSpace)g_nextHandle++; return XR_SUCCESS; }
XrResult XRAPI_CALL DestroySpace(XrSpace) { return XR_SUCCESS; }
XrResult XRAPI_CALL ResultToString(XrInstance, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]) { sprintf_s(buf, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", (int)r); return XR_SUCCESS; }
XrResult XRAPI_CALL StructureTypeToString(XrInstance, XrStructureType t, char buf[XR_MAX_STRUCTURE_NAME_SIZE]) { sprintf_s(buf, XR_MAX_STRUCTURE_NAME_SIZE, "XR_TYPE_%d", (int)t); return XR_SUCCESS; }

XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* fn);

struct Entry { const char* name; PFN_xrVoidFunction fn; };
#define E(n, f) { n, (PFN_xrVoidFunction)f }
const Entry kTable[] = {
    E("xrGetInstanceProcAddr", GetInstanceProcAddr),
    E("xrEnumerateInstanceExtensionProperties", EnumerateInstanceExtensionProperties),
    E("xrCreateInstance", CreateInstance), E("xrDestroyInstance", DestroyInstance),
    E("xrGetInstanceProperties", GetInstanceProperties), E("xrGetSystem", GetSystem),
    E("xrGetSystemProperties", GetSystemProperties), E("xrGetD3D11GraphicsRequirementsKHR", GetD3D11Req),
    E("xrGetD3D12GraphicsRequirementsKHR", GetD3D12Req), E("xrGetDisplayRefreshRateFB", GetDisplayRefreshRateFB),
    E("xrCreateSession", CreateSession), E("xrDestroySession", DestroySession), E("xrBeginSession", BeginSession),
    E("xrEndSession", EndSession), E("xrPollEvent", PollEvent), E("xrWaitFrame", WaitFrame), E("xrBeginFrame", BeginFrame),
    E("xrLocateViews", LocateViews),
    E("xrEndFrame", EndFrame), E("xrEnumerateSwapchainFormats", EnumerateSwapchainFormats),
    E("xrCreateSwapchain", CreateSwapchain), E("xrDestroySwapchain", DestroySwapchain),
    E("xrEnumerateSwapchainImages", EnumerateSwapchainImages), E("xrAcquireSwapchainImage", AcquireSwapchainImage),
    E("xrWaitSwapchainImage", WaitSwapchainImage), E("xrReleaseSwapchainImage", ReleaseSwapchainImage),
    E("xrCreateReferenceSpace", CreateReferenceSpace), E("xrDestroySpace", DestroySpace),
    E("xrResultToString", ResultToString), E("xrStructureTypeToString", StructureTypeToString),
};

XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
    for (const Entry& e : kTable) if (strcmp(e.name, name) == 0) { *fn = e.fn; return XR_SUCCESS; }
    *fn = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
} // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* li,
                                                                                        XrNegotiateRuntimeRequest* req) {
    if (!li || !req || li->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO) return XR_ERROR_INITIALIZATION_FAILED;
    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    req->getInstanceProcAddr = GetInstanceProcAddr;
    return XR_SUCCESS;
}
