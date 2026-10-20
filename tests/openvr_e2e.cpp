// openvr_e2e -- a fake SteamVR (OpenVR) game for testing the VR EyeCapture proxy without SteamVR or a headset.
//
// Loads <game-dir>\openvr_api.dll (the proxy; tests/run_steamvr.ps1 puts tests/mock_openvr.dll next to it as
// openvr_api_real.dll), checks the interface-accessor jump stubs, calls VR_InitInternal2, asks for an IVRCompositor (C++
// object or "FnTable:" table) and runs a D3D11 (or, --api d3d12, a D3D12) frame loop at 90 Hz: WaitGetPoses, draw
// tests/test_pattern.h into each eye, Submit(Left), Submit(Right), then VR_ShutdownInternal. The proxy records through XR_EYE_CAPTURE_AUTORECORD (set by the
// script). Every Submit's result must be what the mock returns (MOCK_OPENVR_SUBMIT_ERROR or None) and the mock must have
// received exactly the game's Submits (MockOpenVrCounts of the real DLL).
//
//   openvr_e2e --game-dir <dir> [--layout two|wide|array|msaa|typeless|fp16|r10|bgrx] [--iface cpp|fntable] [--version 028|022|099]
//              [--pose submit|wait|none] [--seconds 5] [--submit-error <n>] [--out-dir <dir>] [--right-first]
//              [--eye 1920x1080] [--cycles N] [--threads] [--api d3d11|d3d12]
// --api d3d12: the eye textures are D3D12 resources submitted as TextureType_DirectX12 (D3D12TextureData_t: resource + the
// direct queue they were rendered on), handed over in PIXEL_SHADER_RESOURCE like Valve's hellovr_dx12: each frame the game
// takes them PIXEL_SHADER_RESOURCE -> RENDER_TARGET (or COPY_DEST for the sRGB scratch copy) -> PIXEL_SHADER_RESOURCE on its
// queue, executes, then submits. Layouts two / wide / array / msaa / typeless / fp16 / r10 (not bgrx). With the D3D12 debug
// layer installed it runs with it on and fails on any D3D12 error message (a resource left in another state than the game
// handed over shows up as a barrier state mismatch at the game's next frame); prints E2E_D3D12_DEBUG=on|unavailable errors <n>.
// --cycles N: N VR sessions in one process (VR_InitInternal2 -> frames -> VR_ShutdownInternal, N times; the proxy must re-arm
// its hooks each time and record again). --threads: WaitGetPoses runs on its own thread (90 Hz), Submit on the main thread,
// as threaded engines do. Prints E2E_MP4S=<n> (MP4s in --out-dir).
// --pose submit: the game submits VRTextureWithPose_t (Submit_TextureWithPose) and the mock reports no WaitGetPoses pose;
// wait: plain Texture_t, the mock's WaitGetPoses pose; none: plain Texture_t and no pose (MOCK_OPENVR_NO_POSE=1).
// Prints E2E_SYM=<w>x<h> (the symmetric_fov frame the proxy should record from the mock's projection), E2E_OUTPUT=<mp4>
// (the newest MP4 in --out-dir, if any), E2E_COUNTS ... and "openvr_e2e PASS" / "FAIL: ..." (exit 0 / 1).
#include <windows.h>
#include <timeapi.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "../third_party/openvr/openvr.h"
#include "head_smoothing.h"
#include "test_motion.h"
#include "test_pattern.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    std::wstring gameDir, outDir;
    std::string layout = "two", iface = "cpp", version = "028", pose = "wait", api = "d3d11";
    double seconds = 5;
    int submitError = 0;
    bool rightFirst = false, threads = false;
    int cycles = 1;
    UINT eyeW = 1920, eyeH = 1080;
};

std::vector<std::string> g_fails;
void Fail(const std::string& s) { g_fails.push_back(s); printf("  FAIL %s\n", s.c_str()); }

ComPtr<IDXGIAdapter1> PickAdapter() {
    ComPtr<IDXGIFactory1> f;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> best, a;
    SIZE_T bestMem = 0;
    for (UINT i = 0; f && SUCCEEDED(f->EnumAdapters1(i, &a)); ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (d.DedicatedVideoMemory > bestMem) { bestMem = d.DedicatedVideoMemory; best = a; }
    }
    return best;
}

// One eye texture of the game (+ a TYPELESS scratch with a UNORM RTV when the texture itself is *_SRGB-typed: the pattern
// bytes are written exactly, then copied).
struct EyeTex {
    ComPtr<ID3D11Texture2D> tex, scratch;
    std::vector<ComPtr<ID3D11RenderTargetView>> rtv;   // per array slice, on scratch (if any) else tex
};

// The D3D12 game: one direct queue, one command list, a fence per frame (the CPU waits for the previous frame before reusing
// the allocator, which is plenty for a test). Eye textures live in PIXEL_SHADER_RESOURCE between frames (the OpenVR hand-off
// state); scratch textures in RENDER_TARGET.
struct EyeTex12 {
    ComPtr<ID3D12Resource> tex, scratch;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtv;   // per array slice, on scratch (if any) else tex
};
struct Game12 {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0;
    bool debug = false;
    std::vector<EyeTex12> eyes;

    void WaitIdle() {
        if (!fence) return;
        queue->Signal(fence.Get(), ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, event);
            WaitForSingleObject(event, 5000);
        }
    }
    static D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a;
        x.Transition.StateAfter = b;
        return x;
    }
    bool Init(IDXGIAdapter1* adapter, DXGI_FORMAT texFmt, DXGI_FORMAT rtvFmt, DXGI_FORMAT scratchFmt, UINT texW, UINT texH,
              UINT arraySize, UINT samples, size_t nTex) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); debug = true; }
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
        ComPtr<ID3D12InfoQueue> iq;
        if (debug && SUCCEEDED(dev.As(&iq))) {   // keep only errors (the count is the test); never break into a debugger
            D3D12_MESSAGE_SEVERITY deny[] = {D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE, D3D12_MESSAGE_SEVERITY_WARNING};
            D3D12_INFO_QUEUE_FILTER f{};
            f.DenyList.NumSeverities = (UINT)(sizeof(deny) / sizeof(deny[0]));
            f.DenyList.pSeverityList = deny;
            iq->PushStorageFilter(&f);
        } else {
            debug = false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        list->Close();
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = (UINT)nTex * arraySize;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)))) return false;
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        eyes.resize(nTex);
        for (EyeTex12& et : eyes) {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = texW; rd.Height = texH; rd.DepthOrArraySize = (UINT16)arraySize; rd.MipLevels = 1; rd.Format = texFmt;
            rd.SampleDesc.Count = samples; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&et.tex))))
                return false;
            ID3D12Resource* target = et.tex.Get();
            if (scratchFmt != DXGI_FORMAT_UNKNOWN) {
                rd.Format = scratchFmt;
                if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                       IID_PPV_ARGS(&et.scratch))))
                    return false;
                target = et.scratch.Get();
            }
            for (UINT sl = 0; sl < arraySize; ++sl) {
                D3D12_RENDER_TARGET_VIEW_DESC rv{};
                rv.Format = rtvFmt;
                if (samples > 1) { rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY; rv.Texture2DMSArray.FirstArraySlice = sl; rv.Texture2DMSArray.ArraySize = 1; }
                else { rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; rv.Texture2DArray.FirstArraySlice = sl; rv.Texture2DArray.ArraySize = 1; }
                dev->CreateRenderTargetView(target, &rv, h);
                et.rtv.push_back(h);
                h.ptr += inc;
            }
        }
        return true;
    }
    // Draws frame f of the pattern into both eyes and executes it; the eye textures end in PIXEL_SHADER_RESOURCE.
    void Draw(uint32_t f, bool wide, bool arr, UINT eyeW, UINT eyeH, std::vector<PatRect>& pat) {
        if (fence->GetCompletedValue() < fenceValue) {   // the previous frame's list still runs: its allocator is busy
            fence->SetEventOnCompletion(fenceValue, event);
            WaitForSingleObject(event, 5000);
        }
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        std::vector<D3D12_RESOURCE_BARRIER> b;
        for (EyeTex12& et : eyes)
            b.push_back(Tr(et.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           et.scratch ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET));
        list->ResourceBarrier((UINT)b.size(), b.data());
        for (int e = 0; e < 2; ++e) {
            EyeTex12& et = eyes[(wide || arr) ? 0 : (size_t)e];
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = et.rtv[arr ? (size_t)e : 0];
            BuildPattern(pat, e, f, wide ? e * (int)eyeW : 0, 0, (int)eyeW, (int)eyeH);
            for (auto& p : pat) list->ClearRenderTargetView(rtv, p.c, 1, &p.r);
        }
        b.clear();
        for (EyeTex12& et : eyes)
            if (et.scratch) b.push_back(Tr(et.scratch.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
        if (!b.empty()) list->ResourceBarrier((UINT)b.size(), b.data());
        for (EyeTex12& et : eyes) if (et.scratch) list->CopyResource(et.tex.Get(), et.scratch.Get());
        b.clear();
        for (EyeTex12& et : eyes) {
            if (et.scratch) b.push_back(Tr(et.scratch.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
            b.push_back(Tr(et.tex.Get(), et.scratch ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        }
        list->ResourceBarrier((UINT)b.size(), b.data());
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        queue->ExecuteCommandLists(1, l);
        queue->Signal(fence.Get(), ++fenceValue);
    }
    // D3D12 debug-layer errors stored so far (prints the first few).
    int Errors() {
        ComPtr<ID3D12InfoQueue> iq;
        if (!debug || FAILED(dev.As(&iq))) return 0;
        const UINT64 n = iq->GetNumStoredMessages();
        int errors = 0;
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<char> buf(len);
            D3D12_MESSAGE* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (FAILED(iq->GetMessage(i, m, &len))) continue;
            if (m->Severity != D3D12_MESSAGE_SEVERITY_ERROR && m->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION) continue;
            if (++errors <= 5) printf("  D3D12 debug layer: %.*s\n", (int)m->DescriptionByteLength, m->pDescription);
        }
        return errors + (int)iq->GetNumMessagesDiscardedByMessageCountLimit();
    }
};

std::wstring NewestMp4(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.mp4").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;
    FILETIME bt{};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) { bt = fd.ftLastWriteTime; best = dir + L"\\" + fd.cFileName; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

void HeadMatrix(double t, vr::HmdMatrix34_t& m) {
    float r[9];
    test_motion::Matrix(test_motion::Head(t), r);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m.m[i][j] = r[i * 3 + j];
    m.m[0][3] = 0; m.m[1][3] = 1.6f; m.m[2][3] = 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        auto nextA = [&]() { std::string r; for (wchar_t c : next()) r += (char)c; return r; };   // ASCII options
        if (k == L"--game-dir") a.gameDir = next();
        else if (k == L"--out-dir") a.outDir = next();
        else if (k == L"--layout") a.layout = nextA();
        else if (k == L"--iface") a.iface = nextA();
        else if (k == L"--version") a.version = nextA();
        else if (k == L"--pose") a.pose = nextA();
        else if (k == L"--seconds") a.seconds = _wtof(next().c_str());
        else if (k == L"--submit-error") a.submitError = _wtoi(next().c_str());
        else if (k == L"--right-first") a.rightFirst = true;
        else if (k == L"--threads") a.threads = true;
        else if (k == L"--cycles") a.cycles = _wtoi(next().c_str());
        else if (k == L"--eye") { const std::string v = nextA(); sscanf_s(v.c_str(), "%ux%u", &a.eyeW, &a.eyeH); }
        else if (k == L"--api") a.api = nextA();
        else { printf("unknown argument %ls\n", k.c_str()); return 2; }
    }
    if (a.gameDir.empty()) { printf("--game-dir is required\n"); return 2; }
    const bool wide = a.layout == "wide", arr = a.layout == "array", msaa = a.layout == "msaa";
    const bool typeless = a.layout == "typeless", fp16 = a.layout == "fp16", r10 = a.layout == "r10", bgrx = a.layout == "bgrx";
    if (!(a.layout == "two" || wide || arr || msaa || typeless || fp16 || r10 || bgrx)) { printf("bad --layout\n"); return 2; }
    const bool d3d12 = a.api == "d3d12";
    if (!(d3d12 || a.api == "d3d11") || (d3d12 && bgrx)) { printf("bad --api (d3d11 | d3d12; d3d12 has no bgrx layout)\n"); return 2; }
    printf("openvr_e2e: %s, layout %s, iface %s, IVRCompositor_%s, pose %s, %.1f s, eye %ux%u%s%s\n", d3d12 ? "D3D12" : "D3D11",
           a.layout.c_str(), a.iface.c_str(), a.version.c_str(), a.pose.c_str(), a.seconds, a.eyeW, a.eyeH,
           a.submitError ? ", submit error expected" : "", a.rightFirst ? ", right eye first" : "");
    if (!a.outDir.empty()) {
        CreateDirectoryW(a.outDir.c_str(), nullptr);
        SetEnvironmentVariableW(L"XR_EYE_CAPTURE_OUTPUT_DIR", a.outDir.c_str());
    }
    if (a.submitError) SetEnvironmentVariableW(L"MOCK_OPENVR_SUBMIT_ERROR", std::to_wstring(a.submitError).c_str());
    SetEnvironmentVariableW(L"MOCK_OPENVR_NO_POSE", a.pose == "wait" ? nullptr : L"1");

    // ---- the proxy ----
    const std::wstring proxyPath = a.gameDir + L"\\openvr_api.dll";
    HMODULE proxy = LoadLibraryW(proxyPath.c_str());
    if (!proxy) { printf("FAIL: cannot load %ls (error %lu)\n", proxyPath.c_str(), GetLastError()); return 1; }
    auto P = [&](const char* n) { FARPROC f = GetProcAddress(proxy, n); if (!f) Fail(std::string("proxy has no export ") + n); return f; };
    auto pInit2 = (PFN_VR_InitInternal2)P("VR_InitInternal2");
    auto pShutdown = (PFN_VR_ShutdownInternal)P("VR_ShutdownInternal");
    auto pGet = (PFN_VR_GetGenericInterface)P("VR_GetGenericInterface");
    auto pHmd = (PFN_VR_IsHmdPresent)P("VR_IsHmdPresent");
    auto pSym = (PFN_VR_GetVRInitErrorAsSymbol)P("VR_GetVRInitErrorAsSymbol");
    auto pRtPath = (PFN_VR_GetRuntimePath)P("VR_GetRuntimePath");
    typedef double (*PFN_Liquid)(double, double, double, double);
    typedef uint64_t (*PFN_Paths)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    typedef void* (*PFN_Acc)();
    auto pLiquid = (PFN_Liquid)P("LiquidVR");
    auto pPaths = (PFN_Paths)P("VRPaths");
    auto pHeadset = (PFN_Acc)P("VRHeadsetView");
    auto pOverlayView = (PFN_Acc)P("VROverlayView");   // one of the three older-DLL accessors (stub index 12, the last)
    auto pVer = (const char* (*)())P("EyeCaptureProxyVersion");
    if (!pInit2 || !pShutdown || !pGet || !pLiquid || !pPaths || !pHeadset || !pOverlayView || !pVer) { printf("openvr_e2e FAIL: exports\n"); return 1; }

    // accessor stubs FIRST (slow path: the stub itself makes the proxy load the real DLL), then again (fast path)
    const double liq = pLiquid(1.0, 2.0, 3.0, 4.0);
    const uint64_t paths = pPaths(1, 2, 3, 4, 5);
    const double liq2 = pLiquid(5.0, 6.0, 7.0, 8.0);
    void* hv = pHeadset();
    void* ov = pOverlayView();
    HMODULE real = GetModuleHandleW(L"openvr_api_real.dll");
    void* (*dummy)() = real ? (void* (*)())GetProcAddress(real, "MockOpenVrDummyAccessor") : nullptr;
    printf("accessor stubs: LiquidVR(1,2,3,4) = %.0f, VRPaths(1..5) = %llu, LiquidVR(5,6,7,8) = %.0f, VRHeadsetView() = %p; proxy v%s\n",
           liq, (unsigned long long)paths, liq2, hv, pVer());
    if (liq != 1234.0 || liq2 != 5678.0) Fail("LiquidVR through the jump stub (xmm0..3 not preserved?)");
    if (paths != 12345) Fail("VRPaths through the jump stub (integer / stack arguments not preserved?)");
    if (!dummy || hv != dummy()) Fail("VRHeadsetView through the jump stub");
    if (!dummy || ov != dummy()) Fail("VROverlayView through the jump stub (older-DLL accessor)");
    if (!pHmd || !pHmd()) Fail("VR_IsHmdPresent");
    char rt[260] = "";
    uint32_t need = 0;
    if (!pRtPath || !pRtPath(rt, sizeof(rt), &need) || strcmp(rt, "C:\\mock\\SteamVR") != 0) Fail("VR_GetRuntimePath");

    // ---- D3D11 ----
    ComPtr<IDXGIAdapter1> adapter = PickAdapter();
    if (!adapter) { printf("openvr_e2e FAIL: no hardware adapter\n"); return 1; }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (!d3d12 && FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                           D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        printf("openvr_e2e FAIL: D3D11CreateDevice\n");
        return 1;
    }
    if (ctx) ctx.As(&ctx1);
    // texture format / RTV format / colour space per layout
    DXGI_FORMAT texFmt = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, rtvFmt = DXGI_FORMAT_B8G8R8A8_UNORM, scratchFmt = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    vr::EColorSpace cs = vr::ColorSpace_Auto;
    if (wide) { texFmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rtvFmt = DXGI_FORMAT_R8G8B8A8_UNORM; scratchFmt = DXGI_FORMAT_R8G8B8A8_TYPELESS; }
    if (msaa) { texFmt = DXGI_FORMAT_B8G8R8A8_TYPELESS; scratchFmt = DXGI_FORMAT_UNKNOWN; cs = vr::ColorSpace_Gamma; }
    if (typeless) { texFmt = DXGI_FORMAT_R8G8B8A8_TYPELESS; rtvFmt = DXGI_FORMAT_R8G8B8A8_UNORM; scratchFmt = DXGI_FORMAT_UNKNOWN; }
    if (fp16) { texFmt = DXGI_FORMAT_R16G16B16A16_FLOAT; rtvFmt = texFmt; scratchFmt = DXGI_FORMAT_UNKNOWN; }
    if (r10) { texFmt = DXGI_FORMAT_R10G10B10A2_UNORM; rtvFmt = texFmt; scratchFmt = DXGI_FORMAT_UNKNOWN; }   // 10-bit, linear
    if (bgrx) { texFmt = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; rtvFmt = DXGI_FORMAT_B8G8R8X8_UNORM; scratchFmt = DXGI_FORMAT_B8G8R8X8_TYPELESS; }
    const UINT samples = msaa ? 4 : 1, arraySize = arr ? 2 : 1, texW = wide ? 2 * a.eyeW : a.eyeW;
    std::vector<EyeTex> eyes(d3d12 ? 0 : (wide || arr ? 1 : 2));
    Game12 g12;
    if (d3d12) {
        if (!g12.Init(adapter.Get(), texFmt, rtvFmt, scratchFmt, texW, a.eyeH, arraySize, samples, wide || arr ? 1 : 2)) {
            printf("openvr_e2e FAIL: D3D12 device / eye textures\n");
            return 1;
        }
        printf("D3D12 game: direct queue %p, %zu eye texture(s), debug layer %s\n", (void*)g12.queue.Get(), g12.eyes.size(),
               g12.debug ? "on" : "not available");
    }
    for (EyeTex& et : eyes) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = texW; d.Height = a.eyeH; d.MipLevels = 1; d.ArraySize = arraySize; d.Format = texFmt;
        d.SampleDesc.Count = samples; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &et.tex))) { printf("openvr_e2e FAIL: eye texture\n"); return 1; }
        ID3D11Texture2D* target = et.tex.Get();
        if (scratchFmt != DXGI_FORMAT_UNKNOWN) {
            d.Format = scratchFmt;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &et.scratch))) { printf("openvr_e2e FAIL: scratch texture\n"); return 1; }
            target = et.scratch.Get();
        }
        for (UINT sl = 0; sl < arraySize; ++sl) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = rtvFmt;
            if (samples > 1) { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY; rd.Texture2DMSArray.FirstArraySlice = sl; rd.Texture2DMSArray.ArraySize = 1; }
            else { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; rd.Texture2DArray.FirstArraySlice = sl; rd.Texture2DArray.ArraySize = 1; }
            ComPtr<ID3D11RenderTargetView> v;
            if (FAILED(dev->CreateRenderTargetView(target, &rd, &v))) { printf("openvr_e2e FAIL: RTV\n"); return 1; }
            et.rtv.push_back(v);
        }
    }

    long long gameSubmits[2] = {0, 0}, badResults = 0;
    std::atomic<long long> waitCalls{0};
    for (int cycle = 0; cycle < (a.cycles > 0 ? a.cycles : 1); ++cycle) {
    if (a.cycles > 1) printf("--- VR session %d of %d\n", cycle + 1, a.cycles);
    // ---- VR init + compositor ----
    vr::EVRInitError ie = vr::VRInitError_Unknown;
    const uint32_t token = pInit2(&ie, vr::VRApplication_Scene, nullptr);
    printf("VR_InitInternal2 -> token %u, error %d (%s)\n", token, (int)ie, pSym ? pSym(ie) : "?");
    if (ie != vr::VRInitError_None) { printf("openvr_e2e FAIL: init\n"); return 1; }
    const std::string ifName = (a.iface == "fntable" ? std::string("FnTable:") : std::string()) + "IVRCompositor_" + a.version;
    vr::EVRInitError ge = vr::VRInitError_Unknown;
    void* comp = pGet(ifName.c_str(), &ge);
    printf("VR_GetGenericInterface(%s) -> %p, error %d\n", ifName.c_str(), comp, (int)ge);
    if (!comp || ge != vr::VRInitError_None) { printf("openvr_e2e FAIL: no compositor\n"); return 1; }
    vr::IVRCompositor* cpp = a.iface == "cpp" ? static_cast<vr::IVRCompositor*>(comp) : nullptr;
    void* const* fn = a.iface == "fntable" ? static_cast<void* const*>(comp) : nullptr;
    const vr::PFN_FnWaitGetPoses fnWait = fn ? (vr::PFN_FnWaitGetPoses)fn[2] : nullptr;
    // IVRCompositor_029 inserted GetSubmitTexture at 5: Submit is slot 6 there (C++ vtable and FnTable alike)
    const int slotSubmit = a.version == "029" ? 6 : 5;
    const vr::PFN_FnSubmit fnSubmit = fn ? (vr::PFN_FnSubmit)fn[slotSubmit] : nullptr;
    typedef vr::EVRCompositorError (*PFN_CppSubmit)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
    const PFN_CppSubmit cppSubmit029 = cpp && slotSubmit == 6 ? (PFN_CppSubmit)(*(void***)cpp)[6] : nullptr;   // not in our _028 header
    // a second ask for the same interface must give a working object too (vtable patched once)
    if (cpp) { void* again = pGet(ifName.c_str(), &ge); if (again != comp) Fail("second VR_GetGenericInterface returned another object"); }

    // ---- frame loop ----
    timeBeginPeriod(1);
    const int fps = 90;
    const int frames = (int)(a.seconds * fps + 0.5);
    LARGE_INTEGER qf, q0;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    std::vector<PatRect> pat;
    vr::TrackedDevicePose_t render[16], gamePoses[16];
    std::atomic<bool> stopPoses{false};
    std::thread poser;
    if (a.threads) {   // WaitGetPoses on its own thread at 90 Hz
        poser = std::thread([&] {
            vr::TrackedDevicePose_t r2[16], g2[16];
            for (int k = 0; !stopPoses.load(); ++k) {
                const vr::EVRCompositorError we = cpp ? cpp->WaitGetPoses(r2, 16, g2, 16) : fnWait(r2, 16, g2, 16);
                ++waitCalls;
                if (we != vr::VRCompositorError_None) Fail("WaitGetPoses error " + std::to_string((int)we));
                Sleep(11);
            }
        });
        for (int k = 0; k < 200 && waitCalls.load() == 0; ++k) Sleep(5);   // a game renders its first frame after its first poses
    }
    for (int f = 0; f < frames; ++f) {
        const double due = (double)f / fps;
        for (;;) {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            const double t = (double)(c.QuadPart - q0.QuadPart) / qf.QuadPart;
            if (t >= due) break;
            if (due - t > 0.002) Sleep(1);
        }
        if (!a.threads) {
            vr::EVRCompositorError we = cpp ? cpp->WaitGetPoses(render, 16, gamePoses, 16) : fnWait(render, 16, gamePoses, 16);
            ++waitCalls;
            if (we != vr::VRCompositorError_None) Fail("WaitGetPoses error " + std::to_string((int)we));
        }
        // draw
        if (d3d12) g12.Draw((uint32_t)f, wide, arr, a.eyeW, a.eyeH, pat);
        for (int e = 0; !d3d12 && e < 2; ++e) {
            EyeTex& et = eyes[(wide || arr) ? 0 : (size_t)e];
            ID3D11RenderTargetView* rtv = et.rtv[arr ? (size_t)e : 0].Get();
            BuildPattern(pat, e, (uint32_t)f, wide ? e * (int)a.eyeW : 0, 0, (int)a.eyeW, (int)a.eyeH);
            for (auto& p : pat) ctx1->ClearView(rtv, p.c, &p.r, 1);
        }
        for (EyeTex& et : eyes) if (et.scratch) ctx->CopyResource(et.tex.Get(), et.scratch.Get());
        // submit (the pose the frame was "rendered" with: the game's own motion clock for --pose submit)
        vr::HmdMatrix34_t pose{};
        HeadMatrix(due, pose);
        for (int k = 0; k < 2; ++k) {
            const int e = a.rightFirst ? 1 - k : k;
            vr::VRTextureWithPose_t t{};
            vr::D3D12TextureData_t td12{};   // TextureType_DirectX12: the handle points to this (on the stack, as games do)
            if (d3d12) {
                td12.m_pResource = g12.eyes[(wide || arr) ? 0 : (size_t)e].tex.Get();
                td12.m_pCommandQueue = g12.queue.Get();
                td12.m_nNodeMask = 0;
                t.handle = &td12;
                t.eType = vr::TextureType_DirectX12;
            } else {
                t.handle = eyes[(wide || arr) ? 0 : (size_t)e].tex.Get();
                t.eType = vr::TextureType_DirectX;
            }
            t.eColorSpace = cs;
            t.mDeviceToAbsoluteTracking = pose;
            vr::VRTextureBounds_t b{0, 0, 1, 1};
            if (wide) { b.uMin = e ? 0.5f : 0.0f; b.uMax = e ? 1.0f : 0.5f; }
            const vr::EVRSubmitFlags flags = a.pose == "submit" ? vr::Submit_TextureWithPose : vr::Submit_Default;
            const vr::EVRCompositorError r = cppSubmit029 ? cppSubmit029(cpp, (vr::EVREye)e, &t, (wide || f % 2) ? &b : nullptr, flags)
                                             : cpp ? cpp->Submit((vr::EVREye)e, &t, (wide || f % 2) ? &b : nullptr, flags)
                                                   : fnSubmit((vr::EVREye)e, &t, (wide || f % 2) ? &b : nullptr, flags);
            ++gameSubmits[e];
            if ((int)r != a.submitError) ++badResults;
        }
        if (ctx) ctx->Flush();
    }
    if (d3d12) g12.WaitIdle();   // the frames and the proxy's copies on the game queue are done before the VR shutdown
    stopPoses = true;
    if (poser.joinable()) poser.join();
    timeEndPeriod(1);
    printf("frame loop done: %d frames, Submits L %lld R %lld, WaitGetPoses %lld, unexpected Submit results %lld\n", frames,
           gameSubmits[0], gameSubmits[1], waitCalls.load(), badResults);
    if (badResults) Fail("Submit returned something else than the mock's result (" + std::to_string(a.submitError) + ")");

    pShutdown();
    }   // VR sessions

    int64_t mc[10] = {};
    auto counts = real ? (void (*)(int64_t*))GetProcAddress(real, "MockOpenVrCounts") : nullptr;
    if (counts) counts(mc);
    else Fail("MockOpenVrCounts not found (is openvr_api_real.dll the mock?)");
    printf("E2E_COUNTS game L %lld R %lld wait %lld | mock L %lld R %lld wait %lld fntable %lld invalid %lld stubs %lld accessors %lld "
           "shutdowns %lld system %lld\n", gameSubmits[0], gameSubmits[1], waitCalls.load(), (long long)mc[0], (long long)mc[1],
           (long long)mc[2], (long long)mc[3], (long long)mc[4], (long long)mc[5], (long long)mc[6], (long long)mc[7], (long long)mc[8]);
    if (mc[0] != gameSubmits[0] || mc[1] != gameSubmits[1]) Fail("the mock did not get exactly the game's Submits");
    if (mc[2] != waitCalls.load()) Fail("the mock did not get exactly the game's WaitGetPoses calls");
    if (a.iface == "fntable" && mc[3] != gameSubmits[0] + gameSubmits[1]) Fail("FnTable Submits not forwarded 1:1");
    if (a.iface == "cpp" && mc[3] != 0) Fail("C++ Submits arrived through the FnTable");
    if (mc[4] != 0) Fail("the mock saw invalid calls");
    if (mc[5] != 0) Fail("an unhooked compositor slot was called (stub calls)");
    if (mc[7] != (a.cycles > 0 ? a.cycles : 1)) Fail("VR_ShutdownInternal not forwarded exactly once per session");
    if (mc[9] != (d3d12 ? gameSubmits[0] + gameSubmits[1] : 0)) Fail("the mock's D3D12 Submit count differs from the game's");
    if (d3d12) {
        g12.WaitIdle();
        const int errs = g12.Errors();
        printf("E2E_D3D12_DEBUG=%s errors %d\n", g12.debug ? "on" : "unavailable", errs);
        if (errs) Fail("D3D12 debug layer errors (" + std::to_string(errs) + ")");
    }

    // the symmetric_fov frame the proxy should record (the mock's GetProjectionRaw)
    float fovL[4], fovR[4];
    fovL[0] = std::atan(-1.1f); fovL[1] = std::atan(0.9f); fovL[2] = std::atan(1.0f); fovL[3] = std::atan(-0.9f);
    fovR[0] = std::atan(-0.9f); fovR[1] = std::atan(1.1f); fovR[2] = std::atan(1.0f); fovR[3] = std::atan(-0.9f);
    const SymmetricFov sym = ComputeSymmetricFov(fovL, fovR, a.eyeW, a.eyeH, a.eyeW, a.eyeH);
    printf("E2E_SYM=%ux%u\n", 2 * sym.eyeW, sym.eyeH);
    if (!a.outDir.empty()) {
        const std::wstring mp4 = NewestMp4(a.outDir);
        if (!mp4.empty()) printf("E2E_OUTPUT=%ls\n", mp4.c_str());
        int n = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((a.outDir + L"\\*.mp4").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { ++n; } while (FindNextFileW(h, &fd)); FindClose(h); }
        printf("E2E_MP4S=%d\n", n);
    }
    if (!g_fails.empty()) { printf("openvr_e2e FAIL: %s\n", g_fails[0].c_str()); return 1; }
    printf("openvr_e2e PASS\n");
    return 0;
}
