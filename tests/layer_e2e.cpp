// layer_e2e -- end-to-end test of the installed layer: real OpenXR loader -> XR_APILAYER_JRDEVO_eye_capture ->
// mock_runtime (XR_RUNTIME_JSON), driving a normal app frame loop (xrWaitFrame / xrBeginFrame / acquire / draw /
// release / xrEndFrame with a projection layer) for each swapchain layout the layer must handle. The layer records via
// XR_EYE_CAPTURE_AUTORECORD (starts at the first frame, stops after N s) into XR_EYE_CAPTURE_OUTPUT_DIR.
//
//   layer_e2e [--api d3d11|d3d12] [--layout two|array|wide|msaa] [--eye 1920x1080] [--seconds 6] [--record 4]
//             [--out-dir dir] [--expect-dormant]
// Prints the path of the produced MP4 ("E2E_OUTPUT=<path>"). Exit code 0 = a new MP4 appeared
// (--expect-dormant: the layer must NOT record, e.g. no tray running; exit 0 = loop completed and no MP4 appeared).
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "test_pattern.h"

using Microsoft::WRL::ComPtr;

namespace {
std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

#define CHECK(x) do { XrResult _r = (x); if (XR_FAILED(_r)) { printf("FAILED %s -> %d\n", #x, (int)_r); return 1; } } while (0)

struct Sc {
    XrSwapchain h = XR_NULL_HANDLE;
    uint32_t w = 0, hgt = 0, arr = 1;
    std::vector<ComPtr<ID3D11RenderTargetView>> rtv11;   // [image * arr + slice]
    std::vector<ComPtr<ID3D12Resource>> img12;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtv12;     // [image * arr + slice]
};
} // namespace

int main(int argc, char** argv) {
    std::string api = "d3d11", layout = "two";
    unsigned eyeW = 1920, eyeH = 1080;
    double seconds = 6, record = 4;
    bool expectDormant = false;
    std::wstring outDir = ExeDir() + L"\\e2e_out";
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (k == "--api") api = next();
        else if (k == "--layout") layout = next();
        else if (k == "--eye") { std::string v = next(); sscanf_s(v.c_str(), "%ux%u", &eyeW, &eyeH); }
        else if (k == "--seconds") seconds = atof(next().c_str());
        else if (k == "--record") record = atof(next().c_str());
        else if (k == "--out-dir") outDir = Widen(next());
        else if (k == "--expect-dormant") expectDormant = true;
        else { printf("unknown argument %s\n", k.c_str()); return 2; }
    }
    const bool d3d12 = api == "d3d12";
    const bool arr = layout == "array", wide = layout == "wide", msaa = layout == "msaa";
    CreateDirectoryW(outDir.c_str(), nullptr);
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", (ExeDir() + L"\\mock_runtime.json").c_str());
    SetEnvironmentVariableW(L"XR_EYE_CAPTURE_OUTPUT_DIR", outDir.c_str());
    wchar_t rec[32];
    swprintf_s(rec, L"%.2f", record);
    SetEnvironmentVariableW(L"XR_EYE_CAPTURE_AUTORECORD", record > 0 ? rec : nullptr);   // --record 0: no autorecord (hotkey / tray test)
    printf("layer_e2e: api=%s layout=%s eye=%ux%u seconds=%.1f record=%.1f\n", api.c_str(), layout.c_str(), eyeW, eyeH, seconds, record);
    const ULONGLONG startTime = GetTickCount64();
    FILETIME ftStart;
    GetSystemTimeAsFileTime(&ftStart);

    // ---- instance ----
    const char* exts[] = {d3d12 ? "XR_KHR_D3D12_enable" : "XR_KHR_D3D11_enable", "XR_FB_display_refresh_rate"};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "eye_capture_e2e");
    strcpy_s(ci.applicationInfo.engineName, "test");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = exts;
    XrInstance inst = XR_NULL_HANDLE;
    CHECK(xrCreateInstance(&ci, &inst));
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(inst, &ip);
    printf("runtime: %s\n", ip.runtimeName);
    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    CHECK(xrGetSystem(inst, &sg, &sys));

    // ---- device ----
    LUID luid{};
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11DeviceContext1> ctx11_1;
    ComPtr<ID3D12Device> dev12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocs(3);
    std::vector<uint64_t> allocVal(3, 0);
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fenceVal = 0;
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d3d12) {
        PFN_xrGetD3D11GraphicsRequirementsKHR req = nullptr;
        CHECK(xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&req));
        XrGraphicsRequirementsD3D11KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHECK(req(inst, sys, &gr));
        luid = gr.adapterLuid;
    } else {
        PFN_xrGetD3D12GraphicsRequirementsKHR req = nullptr;
        CHECK(xrGetInstanceProcAddr(inst, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&req));
        XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        CHECK(req(inst, sys, &gr));
        luid = gr.adapterLuid;
    }
    ComPtr<IDXGIFactory4> fac;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    ComPtr<IDXGIAdapter1> adapter;
    if (!fac || FAILED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) { printf("adapter by LUID failed\n"); return 1; }
    XrGraphicsBindingD3D11KHR b11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrGraphicsBindingD3D12KHR b12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    if (!d3d12) {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                     D3D11_SDK_VERSION, &dev11, nullptr, &ctx11))) { printf("D3D11 device failed\n"); return 1; }
        ctx11.As(&ctx11_1);
        b11.device = dev11.Get();
    } else {
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12)))) { printf("D3D12 device failed\n"); return 1; }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        for (auto& a : allocs) dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0].Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 32;
        dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        b12.device = dev12.Get();
        b12.queue = queue.Get();
    }

    // ---- session ----
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = d3d12 ? (const void*)&b12 : (const void*)&b11;
    sci.systemId = sys;
    XrSession session = XR_NULL_HANDLE;
    CHECK(xrCreateSession(inst, &sci, &session));
    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    CHECK(xrBeginSession(session, &sbi));
    XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1;
    XrSpace space = XR_NULL_HANDLE;
    CHECK(xrCreateReferenceSpace(session, &rsi, &space));

    // ---- swapchains ----
    std::vector<Sc> scs((arr || wide) ? 1 : 2);
    const UINT inc = d3d12 ? dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
    D3D12_CPU_DESCRIPTOR_HANDLE hnd{};
    if (d3d12) hnd = heap->GetCPUDescriptorHandleForHeapStart();
    for (auto& s : scs) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        sc.sampleCount = msaa ? 4 : 1;
        sc.width = wide ? 2 * eyeW : eyeW;
        sc.height = eyeH;
        sc.faceCount = 1;
        sc.arraySize = arr ? 2 : 1;
        sc.mipCount = 1;
        CHECK(xrCreateSwapchain(session, &sc, &s.h));
        s.w = sc.width; s.hgt = sc.height; s.arr = sc.arraySize;
        uint32_t n = 0;
        CHECK(xrEnumerateSwapchainImages(s.h, 0, &n, nullptr));
        if (!d3d12) {
            std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            CHECK(xrEnumerateSwapchainImages(s.h, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
            for (auto& im : imgs) {
                for (uint32_t sl = 0; sl < s.arr; ++sl) {
                    D3D11_RENDER_TARGET_VIEW_DESC rd{};
                    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    if (msaa) { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY; rd.Texture2DMSArray.FirstArraySlice = sl; rd.Texture2DMSArray.ArraySize = 1; }
                    else { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; rd.Texture2DArray.FirstArraySlice = sl; rd.Texture2DArray.ArraySize = 1; }
                    ComPtr<ID3D11RenderTargetView> v;
                    if (FAILED(dev11->CreateRenderTargetView(im.texture, &rd, &v))) { printf("RTV failed\n"); return 1; }
                    s.rtv11.push_back(v);
                }
            }
        } else {
            std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            CHECK(xrEnumerateSwapchainImages(s.h, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
            for (auto& im : imgs) {
                s.img12.push_back(im.texture);
                for (uint32_t sl = 0; sl < s.arr; ++sl) {
                    D3D12_RENDER_TARGET_VIEW_DESC v{};
                    v.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    if (msaa) { v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY; v.Texture2DMSArray.FirstArraySlice = sl; v.Texture2DMSArray.ArraySize = 1; }
                    else { v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray.FirstArraySlice = sl; v.Texture2DArray.ArraySize = 1; }
                    dev12->CreateRenderTargetView(im.texture, &v, hnd);
                    s.rtv12.push_back(hnd);
                    hnd.ptr += inc;
                }
            }
        }
    }

    // ---- frame loop ----
    const uint32_t frames = (uint32_t)(seconds * 90);
    std::vector<PatRect> pat;
    for (uint32_t f = 0; f < frames; ++f) {
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        CHECK(xrWaitFrame(session, &fwi, &fs));
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        CHECK(xrBeginFrame(session, &fbi));
        uint32_t idx[2] = {0, 0};
        for (size_t s = 0; s < scs.size(); ++s) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            CHECK(xrAcquireSwapchainImage(scs[s].h, &ai, &idx[s]));
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            CHECK(xrWaitSwapchainImage(scs[s].h, &wi));
        }
        if (d3d12) {
            const uint32_t k = f % 3;
            if (fence->GetCompletedValue() < allocVal[k]) { fence->SetEventOnCompletion(allocVal[k], evt); WaitForSingleObject(evt, 5000); }
            allocs[k]->Reset();
            list->Reset(allocs[k].Get(), nullptr);
        }
        XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int e = 0; e < 2; ++e) {
            const size_t s = (arr || wide) ? 0 : (size_t)e;
            Sc& sc = scs[s];
            const uint32_t slice = arr ? (uint32_t)e : 0;
            const int ox = wide ? e * (int)eyeW : 0;
            BuildPattern(pat, e, f, ox, 0, (int)eyeW, (int)eyeH);
            const size_t rv = (size_t)idx[s] * sc.arr + slice;
            for (auto& p : pat) {
                if (!d3d12) ctx11_1->ClearView(sc.rtv11[rv].Get(), p.c, &p.r, 1);
                else list->ClearRenderTargetView(sc.rtv12[rv], p.c, 1, &p.r);
            }
            views[e].pose.orientation.w = 1;
            views[e].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
            views[e].subImage.swapchain = sc.h;
            views[e].subImage.imageRect.offset = {ox, 0};
            views[e].subImage.imageRect.extent = {(int32_t)eyeW, (int32_t)eyeH};
            views[e].subImage.imageArrayIndex = slice;
        }
        if (d3d12) {
            list->Close();
            ID3D12CommandList* l[] = {list.Get()};
            queue->ExecuteCommandLists(1, l);
            queue->Signal(fence.Get(), ++fenceVal);
            allocVal[f % 3] = fenceVal;
        }
        for (size_t s = 0; s < scs.size(); ++s) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            CHECK(xrReleaseSwapchainImage(scs[s].h, &ri));
        }
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        proj.space = space;
        proj.viewCount = 2;
        proj.views = views;
        const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&proj};
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 1;
        fei.layers = layers;
        CHECK(xrEndFrame(session, &fei));
    }

    // ---- teardown (the layer finalizes in xrDestroySession if still recording) ----
    if (d3d12) { queue->Signal(fence.Get(), ++fenceVal); fence->SetEventOnCompletion(fenceVal, evt); WaitForSingleObject(evt, 5000); }
    CHECK(xrEndSession(session));
    for (auto& s : scs) CHECK(xrDestroySwapchain(s.h));
    xrDestroySpace(space);
    CHECK(xrDestroySession(session));
    CHECK(xrDestroyInstance(inst));
    CloseHandle(evt);

    // ---- newest mp4 in outDir created after start ----
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((outDir + L"\\*.mp4").c_str(), &fd);
    std::wstring best;
    FILETIME bestT = ftStart;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) { bestT = fd.ftLastWriteTime; best = outDir + L"\\" + fd.cFileName; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    printf("elapsed %.1f s\n", (GetTickCount64() - startTime) / 1000.0);
    if (expectDormant) {
        if (!best.empty()) { printf("E2E FAIL: dormant layer produced %ls\n", best.c_str()); return 1; }
        printf("E2E_DORMANT_OK frames=%u (loop completed, no recording)\n", frames);
        return 0;
    }
    if (best.empty()) { printf("E2E FAIL: no new MP4 in %ls\n", outDir.c_str()); return 1; }
    printf("E2E_OUTPUT=%ls\n", best.c_str());
    return 0;
}
