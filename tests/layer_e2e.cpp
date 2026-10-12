// layer_e2e -- end-to-end test of the installed layer: real OpenXR loader -> XR_APILAYER_JRDEVO_eye_capture ->
// mock_runtime (XR_RUNTIME_JSON), driving a normal app frame loop (xrWaitFrame / xrBeginFrame / acquire / draw /
// release / xrEndFrame with a projection layer) for each swapchain layout the layer must handle. The layer records via
// XR_EYE_CAPTURE_AUTORECORD (starts at the first frame, stops after N s) into XR_EYE_CAPTURE_OUTPUT_DIR.
//
//   layer_e2e [--api d3d11|d3d12] [--layout two|array|wide|msaa] [--eye 1920x1080] [--seconds 6] [--record 4]
//             [--out-dir dir] [--expect-dormant] [--jitter] [--hud] [--overlays] [--stale-pose] [--opaque-projection]
// --overlays (record_overlays test; tests/run_e2e.ps1 has the expected pixel positions): every frame also submits, AFTER
// the projection layer, four overlay layers drawn with clears into their own R8G8B8A8_UNORM_SRGB swapchains:
//   A quad, projection (LOCAL) space, pose (0, 1.45, -2), 0.5 x 0.5 m, opaque green (alpha 255, SOURCE_ALPHA flag)
//   B quad, VIEW space, pose (-0.30, -0.128, -1), 0.2 x 0.08 m, white with alpha 128, SOURCE_ALPHA | UNPREMULTIPLIED
//   C cylinder, VIEW space, pose (0.5, 0.9, 0), radius 1.5 m, central angle 0.4 rad, aspect 4, orange with alpha 0 and
//     NO blend flag (must be opaque: the texture alpha is ignored)
//   D quad, LOCAL space, pose (0.7, 1.41, -1.5) turned 180 degrees about +Y (its back faces the viewer), magenta:
//     must NOT be drawn (quads are one-sided)
//   G quad, LOCAL (projection) space, a NEW pose every frame: x = 0.4 sin(2 pi 0.1 tau), y = 0.228, z = -2 (tau = seconds
//     since the first predictedDisplayTime), 0.2 x 0.1 m, opaque yellow (like the DLAA injector's cab-fixed fps box: a
//     layer in the projection layer's space whose pose is rebuilt at every xrEndFrame)
//   BEFORE the projection layer (which then has BLEND_TEXTURE_SOURCE_ALPHA; the pattern's band A is alpha 0):
//   P an XrCompositionLayerPassthroughFB stub (no swapchain: must be skipped by name), K an opaque cube (6 faces in
//     distinct colours, -Z with a second colour for u < 0.25) turned +45 degrees about +Y, H an opaque purple quad at
//     (-1.864, 2.734, -2), 0.3 x 1.0 m (top under the alpha-0 black grey step, bottom under the opaque red block)
//   after it, also: I equirect2 (infinite, yaw -0.64, 0.12 rad wide, elevation -0.39..-0.27, two coloured halves),
//     J equirect v1 (10 m sphere around the head, scale / bias window around azimuth 0, elevation -0.5, two halves),
//     M 4x MSAA teal quad, F R16G16B16A16_FLOAT quad, T R10G10B10A2_UNORM quad, S white quad with an
//     XrCompositionLayerColorScaleBiasKHR (scale 0.5, 0.25, 0, 1; bias 0, 0, 0.2, 0) -- run_e2e.ps1 has the expected pixels
// --opaque-projection (with --overlays): the projection layer has no blend flag, so P / K / H must be hidden (skipped)
// --stale-pose (implies --overlays): the mock reports the tests/test_motion.h head motion and the app submits the views
// located 33 ms before displayTime (asymmetric mock fovs), while drawing the same clear pattern. VIEW-space layers must
// stay at one pixel position in every frame (they follow the submitted head, not the head at displayTime).
//   E quad, LOCAL space, pose (-1.094, 1.4284, -1.5), 0.2 x 0.06 m, opaque cyan, eyeVisibility LEFT: left eye only
// --jitter (D3D11, layout two): the mock runtime reports the head motion of tests/test_motion.h through xrLocateViews
// and every frame renders a world-locked grid + marker for the located pose / fov and submits that pose / fov, like a
// game. Prints E2E_T0_NS=<first predictedDisplayTime> (t = 0 of the motion). Default eye size 1408x1440.
// --hud (with --jitter; the hud cases of tests/run_jitter.ps1): two opaque quads after the projection layer, each from its
// own 64x32 R8G8B8A8_UNORM_SRGB swapchain cleared every frame: V green (40, 200, 40) in a VIEW space at (0.12, -0.10, -1),
// 0.10 x 0.06 m (head-locked, like the DLAA injector's panel), W magenta (200, 40, 200) in the projection (LOCAL) space
// at (-0.25, 1.75, -2), 0.12 x 0.08 m (world-locked).
// Prints the path of the produced MP4 ("E2E_OUTPUT=<path>"). Exit code 0 = a new MP4 appeared
// (--expect-dormant: the layer must NOT record, e.g. no tray running; exit 0 = loop completed and no MP4 appeared).
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "test_motion.h"
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

const char kSceneVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
// World-locked scene, rendered for the located view each frame (like a game): background 40, a grid line (90) every
// 10 degrees of yaw / pitch, and a white (255) disk of 0.6 degree radius at the world direction -Z (straight ahead at
// t = 0). Values are written through the UNORM RTV, i.e. they are the bytes in the sRGB swapchain.
const char kScenePs[] =
    "cbuffer C : register(b0) { float4 m0, m1, m2, tans, vp; };\n"   // world-from-eye rows; tanL,tanR,tanU,tanD; viewport
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    float2 uv = (pos.xy - vp.xy) / vp.zw;\n"
    "    float3 d = normalize(float3(lerp(tans.x, tans.y, uv.x), lerp(tans.z, tans.w, uv.y), -1.0));\n"
    "    float3 w = float3(dot(m0.xyz, d), dot(m1.xyz, d), dot(m2.xyz, d));\n"
    "    if (-w.z > cos(radians(0.6))) return float4(1.0, 1.0, 1.0, 1.0);\n"
    "    float yaw = degrees(atan2(-w.x, -w.z)), pitch = degrees(asin(clamp(w.y, -1.0, 1.0)));\n"
    "    float gy = abs(frac(yaw / 10.0 + 0.5) - 0.5) * 10.0, gp = abs(frac(pitch / 10.0 + 0.5) - 0.5) * 10.0;\n"
    "    float v = min(gy, gp) < 0.12 ? 90.0 : 40.0;\n"
    "    return float4(v / 255.0, v / 255.0, v / 255.0, 1.0);\n"
    "}\n";
struct SceneCb { float m0[4], m1[4], m2[4], tans[4], vp[4]; };

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
    bool expectDormant = false, jitter = false, eyeSet = false, overlays = false, stalePose = false, opaqueProj = false, hud = false;
    std::wstring outDir = ExeDir() + L"\\e2e_out";
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (k == "--api") api = next();
        else if (k == "--layout") layout = next();
        else if (k == "--eye") { std::string v = next(); sscanf_s(v.c_str(), "%ux%u", &eyeW, &eyeH); eyeSet = true; }
        else if (k == "--jitter") jitter = true;
        else if (k == "--hud") hud = true;
        else if (k == "--overlays") overlays = true;
        else if (k == "--stale-pose") stalePose = overlays = true;
        else if (k == "--opaque-projection") opaqueProj = true;
        else if (k == "--seconds") seconds = atof(next().c_str());
        else if (k == "--record") record = atof(next().c_str());
        else if (k == "--out-dir") outDir = Widen(next());
        else if (k == "--expect-dormant") expectDormant = true;
        else { printf("unknown argument %s\n", k.c_str()); return 2; }
    }
    const bool d3d12 = api == "d3d12";
    const bool arr = layout == "array", wide = layout == "wide", msaa = layout == "msaa";
    if (jitter && (d3d12 || layout != "two")) { printf("--jitter needs --api d3d11 --layout two\n"); return 2; }
    if (jitter && !eyeSet) { eyeW = 1408; eyeH = 1440; }
    if (hud && (!jitter || overlays)) { printf("--hud needs --jitter (and no --overlays)\n"); return 2; }
    SetEnvironmentVariableW(L"EYECAP_MOCK_MOTION", (jitter || stalePose) ? L"jitter" : nullptr);
    CreateDirectoryW(outDir.c_str(), nullptr);
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", (ExeDir() + L"\\mock_runtime.json").c_str());
    SetEnvironmentVariableW(L"XR_EYE_CAPTURE_OUTPUT_DIR", outDir.c_str());
    wchar_t rec[32];
    swprintf_s(rec, L"%.2f", record);
    SetEnvironmentVariableW(L"XR_EYE_CAPTURE_AUTORECORD", record > 0 ? rec : nullptr);   // --record 0: no autorecord (hotkey / tray test)
    printf("layer_e2e: api=%s layout=%s eye=%ux%u seconds=%.1f record=%.1f%s%s%s\n", api.c_str(), layout.c_str(), eyeW, eyeH,
           seconds, record, jitter ? " jitter" : "", hud ? " hud" : "", overlays ? " overlays" : "");
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
        hd.NumDescriptors = 160;
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

    // ---- jitter scene renderer (D3D11) ----
    ComPtr<ID3D11VertexShader> sceneVs;
    ComPtr<ID3D11PixelShader> scenePs;
    ComPtr<ID3D11Buffer> sceneCb;
    if (jitter) {
        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(kSceneVs, sizeof(kSceneVs) - 1, "scene_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &err)) ||
            FAILED(D3DCompile(kScenePs, sizeof(kScenePs) - 1, "scene_ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &err))) {
            printf("scene shader compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "");
            return 1;
        }
        dev11->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &sceneVs);
        dev11->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &scenePs);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(SceneCb);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev11->CreateBuffer(&bd, nullptr, &sceneCb);
        if (!sceneVs || !scenePs || !sceneCb) { printf("scene renderer setup failed\n"); return 1; }
    }

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

    // ---- overlay swapchains (--overlays) ----
    // 0 A green, 1 B white alpha 128, 2 C orange alpha 0, 3 D magenta, 4 E cyan, 5 G yellow (all R8G8B8A8_UNORM_SRGB),
    // 6 H behind-projection quad purple, 7 K cube (6 faces, colours kCubeFace, face 5 has a second colour for u < 0.25),
    // 8 I equirect2 (left half / right half), 9 J equirect v1 (halves), 10 M 4x MSAA teal, 11 F R16G16B16A16_FLOAT
    // linear (0.2, 0.6, 0.2), 12 T R10G10B10A2_UNORM linear (0.8, 0.3, 0.5), 13 S white + XrCompositionLayerColorScaleBiasKHR.
    // Clear colours of the sRGB swapchains are the stored (sRGB-encoded) bytes (UNORM RTV); FP16 / 10-bit are linear.
    struct OvSpec { uint32_t w, h, samples, faces; DXGI_FORMAT fmt, rtv; int split; float c[4], c2[4]; };
    const DXGI_FORMAT kS = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kU = DXGI_FORMAT_R8G8B8A8_UNORM;
    const float k255 = 1.0f / 255.0f;
    const OvSpec ovSpec[] = {
        {64, 64, 1, 1, kS, kU, 0, {0, 1, 0, 1}, {}},
        {64, 32, 1, 1, kS, kU, 0, {1, 1, 1, 128 * k255}, {}},
        {128, 32, 1, 1, kS, kU, 0, {1, 128 * k255, 0, 0}, {}},
        {64, 32, 1, 1, kS, kU, 0, {1, 0, 1, 1}, {}},
        {64, 32, 1, 1, kS, kU, 0, {0, 1, 1, 1}, {}},
        {64, 32, 1, 1, kS, kU, 0, {1, 1, 0, 1}, {}},
        {64, 64, 1, 1, kS, kU, 0, {128 * k255, 0, 1, 1}, {}},
        {64, 64, 1, 6, kS, kU, 2, {}, {90 * k255, 0, 200 * k255, 1}},
        {128, 64, 1, 1, kS, kU, 1, {1, 0, 128 * k255, 1}, {0, 64 * k255, 1, 1}},
        {128, 64, 1, 1, kS, kU, 1, {0, 200 * k255, 200 * k255, 1}, {200 * k255, 0, 200 * k255, 1}},
        {64, 32, 4, 1, kS, kU, 0, {0, 128 * k255, 128 * k255, 1}, {}},
        {64, 32, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, {0.2f, 0.6f, 0.2f, 1}, {}},
        {64, 32, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, 0, {0.8f, 0.3f, 0.5f, 1}, {}},
        {64, 32, 1, 1, kS, kU, 0, {1, 1, 1, 1}, {}},
    };
    // cube faces +X -X +Y -Y +Z -Z (sRGB bytes)
    const float kCubeFace[6][4] = {{0, 0, 160 * k255, 1}, {160 * k255, 160 * k255, 0, 1}, {160 * k255, 0, 160 * k255, 1},
                                   {0, 160 * k255, 160 * k255, 1}, {100 * k255, 100 * k255, 100 * k255, 1}, {0, 160 * k255, 0, 1}};
    const int kOv = (int)(sizeof(ovSpec) / sizeof(ovSpec[0]));
    std::vector<Sc> ovs(overlays ? kOv : 0);
    XrSpace viewSpace = XR_NULL_HANDLE;
    if (overlays) {
        XrReferenceSpaceCreateInfo vsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        vsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        vsi.poseInReferenceSpace.orientation.w = 1;
        CHECK(xrCreateReferenceSpace(session, &vsi, &viewSpace));
        for (int k = 0; k < kOv; ++k) {
            Sc& s = ovs[k];
            const OvSpec& o = ovSpec[k];
            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = o.fmt;
            sc.sampleCount = o.samples; sc.width = o.w; sc.height = o.h; sc.faceCount = o.faces; sc.arraySize = 1; sc.mipCount = 1;
            CHECK(xrCreateSwapchain(session, &sc, &s.h));
            s.w = sc.width; s.hgt = sc.height; s.arr = o.faces;   // RTVs per image: one per face slice
            uint32_t n = 0;
            CHECK(xrEnumerateSwapchainImages(s.h, 0, &n, nullptr));
            if (!d3d12) {
                std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                CHECK(xrEnumerateSwapchainImages(s.h, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
                for (auto& im : imgs) {
                    for (uint32_t sl = 0; sl < o.faces; ++sl) {
                        D3D11_RENDER_TARGET_VIEW_DESC rd{};
                        rd.Format = o.rtv;
                        if (o.samples > 1) rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
                        else { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; rd.Texture2DArray.FirstArraySlice = sl; rd.Texture2DArray.ArraySize = 1; }
                        ComPtr<ID3D11RenderTargetView> v;
                        if (FAILED(dev11->CreateRenderTargetView(im.texture, &rd, &v))) { printf("overlay RTV %d failed\n", k); return 1; }
                        s.rtv11.push_back(v);
                    }
                }
            } else {
                std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                CHECK(xrEnumerateSwapchainImages(s.h, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
                for (auto& im : imgs) {
                    s.img12.push_back(im.texture);
                    for (uint32_t sl = 0; sl < o.faces; ++sl) {
                        D3D12_RENDER_TARGET_VIEW_DESC v{};
                        v.Format = o.rtv;
                        if (o.samples > 1) v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                        else { v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray.FirstArraySlice = sl; v.Texture2DArray.ArraySize = 1; }
                        dev12->CreateRenderTargetView(im.texture, &v, hnd);
                        s.rtv12.push_back(hnd);
                        hnd.ptr += inc;
                    }
                }
            }
        }
        printf("E2E_OVERLAYS=1 (quad LOCAL opaque, quad VIEW alpha 128 unpremultiplied, cylinder VIEW opaque, back-facing quad, "
               "left-eye-only quad, per-frame-pose quad, passthrough FB stub + cube + quad BEFORE the projection layer%s, "
               "equirect2, equirect, 4x MSAA quad, FP16 quad, 10-bit quad, colour scale / bias quad)\n",
               opaqueProj ? " (projection opaque: hidden)" : " (projection with source alpha)");
    }
    // ---- --hud: V (VIEW space, green) and W (LOCAL, magenta) quads, D3D11 ----
    std::vector<Sc> huds(hud ? 2 : 0);
    const float kHudColour[2][4] = {{40 * k255, 200 * k255, 40 * k255, 1}, {200 * k255, 40 * k255, 200 * k255, 1}};
    if (hud) {
        XrReferenceSpaceCreateInfo vsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        vsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        vsi.poseInReferenceSpace.orientation.w = 1;
        CHECK(xrCreateReferenceSpace(session, &vsi, &viewSpace));
        for (Sc& s : huds) {
            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = kS;
            sc.sampleCount = 1; sc.width = 64; sc.height = 32; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
            CHECK(xrCreateSwapchain(session, &sc, &s.h));
            s.w = sc.width; s.hgt = sc.height;
            uint32_t n = 0;
            CHECK(xrEnumerateSwapchainImages(s.h, 0, &n, nullptr));
            std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            CHECK(xrEnumerateSwapchainImages(s.h, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
            for (auto& im : imgs) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = kU;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ComPtr<ID3D11RenderTargetView> v;
                if (FAILED(dev11->CreateRenderTargetView(im.texture, &rd, &v))) { printf("hud RTV failed\n"); return 1; }
                s.rtv11.push_back(v);
            }
        }
        printf("E2E_HUD=1 (quad V VIEW space green, quad W LOCAL magenta)\n");
    }

    // ---- frame loop ----
    const uint32_t frames = (uint32_t)(seconds * 90);
    XrTime t0ns = 0;
    std::vector<PatRect> pat;
    for (uint32_t f = 0; f < frames; ++f) {
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        CHECK(xrWaitFrame(session, &fwi, &fs));
        if (f == 0) { printf("E2E_T0_NS=%lld\n", (long long)fs.predictedDisplayTime); t0ns = fs.predictedDisplayTime; }
        XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        if (jitter || stalePose) {
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            // --stale-pose: the views of 33 ms EARLIER are submitted (a game rendering with an old pose, like ETS2's
            // submitted poses that match no located one): the VIEW space located at displayTime is then a different head
            li.displayTime = fs.predictedDisplayTime - (stalePose ? 33333333 : 0);
            li.space = space;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t nv = 0;
            CHECK(xrLocateViews(session, &li, &vs, 2, &nv, located));
        }
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
        uint32_t ovIdx[32] = {};
        for (size_t k = 0; k < ovs.size(); ++k) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            CHECK(xrAcquireSwapchainImage(ovs[k].h, &ai, &ovIdx[k]));
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            CHECK(xrWaitSwapchainImage(ovs[k].h, &wi));
            const OvSpec& o = ovSpec[k];
            for (uint32_t sl = 0; sl < o.faces; ++sl) {
                const size_t rv = (size_t)ovIdx[k] * o.faces + sl;
                // (colour, rectangle) clears: whole image, then the split part
                const float* c0 = o.split == 2 ? kCubeFace[sl] : o.c;
                D3D12_RECT part{0, 0, 0, 0};
                const float* c1 = nullptr;
                if (o.split == 1) { part = {(LONG)o.w / 2, 0, (LONG)o.w, (LONG)o.h}; c1 = o.c2; }    // right half
                if (o.split == 2 && sl == 5) { part = {0, 0, (LONG)o.w / 4, (LONG)o.h}; c1 = o.c2; }  // -Z face, u < 0.25
                if (!d3d12) {
                    ctx11->ClearRenderTargetView(ovs[k].rtv11[rv].Get(), c0);
                    if (c1) ctx11_1->ClearView(ovs[k].rtv11[rv].Get(), c1, &part, 1);
                } else {
                    list->ClearRenderTargetView(ovs[k].rtv12[rv], c0, 0, nullptr);
                    if (c1) list->ClearRenderTargetView(ovs[k].rtv12[rv], c1, 1, &part);
                }
            }
        }
        for (size_t k = 0; k < huds.size(); ++k) {
            uint32_t hi = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            CHECK(xrAcquireSwapchainImage(huds[k].h, &ai, &hi));
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            CHECK(xrWaitSwapchainImage(huds[k].h, &wi));
            ctx11->ClearRenderTargetView(huds[k].rtv11[hi].Get(), kHudColour[k]);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            CHECK(xrReleaseSwapchainImage(huds[k].h, &ri));
        }
        XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int e = 0; e < 2; ++e) {
            const size_t s = (arr || wide) ? 0 : (size_t)e;
            Sc& sc = scs[s];
            const uint32_t slice = arr ? (uint32_t)e : 0;
            const int ox = wide ? e * (int)eyeW : 0;
            const size_t rv = (size_t)idx[s] * sc.arr + slice;
            if (jitter) {
                const XrQuaternionf& o = located[e].pose.orientation;
                float m[9];
                test_motion::Matrix({o.x, o.y, o.z, o.w}, m);
                const XrFovf& fv = located[e].fov;
                SceneCb cb = {{m[0], m[1], m[2], 0}, {m[3], m[4], m[5], 0}, {m[6], m[7], m[8], 0},
                              {std::tan(fv.angleLeft), std::tan(fv.angleRight), std::tan(fv.angleUp), std::tan(fv.angleDown)},
                              {(float)ox, 0.0f, (float)eyeW, (float)eyeH}};
                ctx11->UpdateSubresource(sceneCb.Get(), 0, nullptr, &cb, 0, 0);
                ID3D11RenderTargetView* rt = sc.rtv11[rv].Get();
                ID3D11Buffer* cbp = sceneCb.Get();
                const D3D11_VIEWPORT vpt{(float)ox, 0.0f, (float)eyeW, (float)eyeH, 0.0f, 1.0f};
                ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx11->VSSetShader(sceneVs.Get(), nullptr, 0);
                ctx11->PSSetShader(scenePs.Get(), nullptr, 0);
                ctx11->PSSetConstantBuffers(0, 1, &cbp);
                ctx11->RSSetViewports(1, &vpt);
                ctx11->OMSetRenderTargets(1, &rt, nullptr);
                ctx11->Draw(3, 0);
                ctx11->OMSetRenderTargets(0, nullptr, nullptr);
                views[e].pose = located[e].pose;
                views[e].fov = located[e].fov;
            } else {
                BuildPattern(pat, e, f, ox, 0, (int)eyeW, (int)eyeH);
                for (auto& p : pat) {
                    if (!d3d12) ctx11_1->ClearView(sc.rtv11[rv].Get(), p.c, &p.r, 1);
                    else list->ClearRenderTargetView(sc.rtv12[rv], p.c, 1, &p.r);
                }
                views[e].pose.orientation.w = 1;
                // --overlays: the real eye positions (the mock's head at (0, 1.6, 0), +-32 mm), so world-locked and
                // VIEW-space layers line up with the views the way a runtime sees them
                if (overlays) views[e].pose.position = {e == 0 ? -0.032f : 0.032f, 1.6f, 0.0f};
                views[e].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
                if (stalePose) { views[e].pose = located[e].pose; views[e].fov = located[e].fov; }
            }
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
        for (size_t k = 0; k < ovs.size(); ++k) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            CHECK(xrReleaseSwapchainImage(ovs[k].h, &ri));
        }
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        proj.space = space;
        proj.viewCount = 2;
        proj.views = views;
        // --overlays: the projection layer has source alpha (band A of the pattern is alpha 0), so the layers before it
        // show through there; --opaque-projection: no flag, they are hidden
        if (overlays && !opaqueProj) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        // overlay layers after the projection layer (back to front)
        XrCompositionLayerQuad qa{XR_TYPE_COMPOSITION_LAYER_QUAD}, qb{XR_TYPE_COMPOSITION_LAYER_QUAD}, qd{XR_TYPE_COMPOSITION_LAYER_QUAD},
            qe{XR_TYPE_COMPOSITION_LAYER_QUAD}, qg{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerCylinderKHR cy{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        auto sub = [&](XrSwapchainSubImage& si, int k) {
            si.swapchain = ovs[k].h;
            si.imageRect.offset = {0, 0};
            si.imageRect.extent = {(int32_t)ovSpec[k].w, (int32_t)ovSpec[k].h};
            si.imageArrayIndex = 0;
        };
        const XrCompositionLayerBaseHeader* layers[32] = {};
        uint32_t nLayers = 0;
        // before the projection layer: P a passthrough layer stub (no swapchain: must be skipped by name), K cube, H quad
        XrCompositionLayerPassthroughFB pt{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerCubeKHR cube{XR_TYPE_COMPOSITION_LAYER_CUBE_KHR};
        XrCompositionLayerQuad qh{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (overlays) {
            pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            pt.space = space;
            pt.layerHandle = (XrPassthroughLayerFB)(uintptr_t)0x5eed;   // never dereferenced by the layer / the mock
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&pt;
            // K: cube, LOCAL space, turned +45 degrees about +Y (the left half of the view sees its -Z face, the right half
            // its +X face, the top its +Y face), opaque
            cube.layerFlags = 0;
            cube.space = space; cube.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cube.swapchain = ovs[7].h; cube.imageArrayIndex = 0;
            cube.orientation = {0, 0.38268343f, 0, 0.92387953f};
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&cube;
            // H: opaque purple quad, LOCAL, centre (-1.864, 2.734, -2), 0.3 x 1.0 m: its top part is under the pattern's
            // alpha-0 black grey step (visible), its lower part under the opaque red block (hidden)
            qh.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            qh.space = space; qh.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            qh.subImage.swapchain = ovs[6].h; qh.subImage.imageRect = {{0, 0}, {(int32_t)ovSpec[6].w, (int32_t)ovSpec[6].h}};
            qh.pose = {{0, 0, 0, 1}, {-1.864f, 2.734f, -2.0f}}; qh.size = {0.3f, 1.0f};
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&qh;
        }
        layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&proj;
        XrCompositionLayerEquirect2KHR eq2{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
        XrCompositionLayerEquirectKHR eq1{XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR};
        XrCompositionLayerQuad qm{XR_TYPE_COMPOSITION_LAYER_QUAD}, qf{XR_TYPE_COMPOSITION_LAYER_QUAD}, qt{XR_TYPE_COMPOSITION_LAYER_QUAD},
            qs{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerColorScaleBiasKHR csb{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
        if (overlays) {
            qa.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            qa.space = space; qa.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(qa.subImage, 0);
            qa.pose = {{0, 0, 0, 1}, {0.0f, 1.45f, -2.0f}}; qa.size = {0.5f, 0.5f};
            qb.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            qb.space = viewSpace; qb.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(qb.subImage, 1);
            qb.pose = {{0, 0, 0, 1}, {-0.30f, -0.128f, -1.0f}}; qb.size = {0.2f, 0.08f};
            cy.layerFlags = 0;
            cy.space = viewSpace; cy.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(cy.subImage, 2);
            cy.pose = {{0, 0, 0, 1}, {0.5f, 0.9f, 0.0f}}; cy.radius = 1.5f; cy.centralAngle = 0.4f; cy.aspectRatio = 4.0f;
            qd.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            qd.space = space; qd.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(qd.subImage, 3);
            qd.pose = {{0, 1, 0, 0}, {0.7f, 1.41f, -1.5f}}; qd.size = {0.2f, 0.08f};   // 180 deg about +Y: back side to us
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&qa;
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&qb;
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&cy;
            qe.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            qe.space = space; qe.eyeVisibility = XR_EYE_VISIBILITY_LEFT; sub(qe.subImage, 4);
            qe.pose = {{0, 0, 0, 1}, {-1.094f, 1.4284f, -1.5f}}; qe.size = {0.2f, 0.06f};
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&qd;
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&qe;
            const double tau = (double)(fs.predictedDisplayTime - t0ns) * 1e-9;
            qg.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            qg.space = space; qg.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(qg.subImage, 5);
            qg.pose = {{0, 0, 0, 1}, {(float)(0.4 * std::sin(2.0 * 3.14159265358979 * 0.1 * tau)), 0.228f, -2.0f}};
            qg.size = {0.2f, 0.1f};
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&qg;
            // I: equirect2, infinitely far (radius 0), LOCAL, turned -0.64 rad about +Y (towards +X): 0.12 rad wide,
            // elevation -0.39 .. -0.27 rad; left half / right half colours
            const XrQuaternionf yawR = {0, std::sin(-0.32f), 0, std::cos(-0.32f)};
            eq2.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            eq2.space = space; eq2.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(eq2.subImage, 8);
            eq2.pose = {yawR, {0, 1.6f, 0}}; eq2.radius = 0.0f;
            eq2.centralHorizontalAngle = 0.12f; eq2.upperVerticalAngle = -0.27f; eq2.lowerVerticalAngle = -0.39f;
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&eq2;
            // J: equirect v1 on a sphere of radius 10 m centred on the head (0, 1.6, 0), same yaw; scale / bias map a window
            // 0.12 rad wide x 0.10 rad high around (azimuth 0, elevation -0.5) onto the image
            const float dH = 0.12f, dV = 0.10f, el0 = -0.5f, kPi = 3.14159265f;
            eq1.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            eq1.space = space; eq1.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(eq1.subImage, 9);
            eq1.pose = {yawR, {0, 1.6f, 0}}; eq1.radius = 10.0f;
            eq1.scale = {2 * kPi / dH, kPi / dV};
            eq1.bias = {0.5f - 0.5f * eq1.scale.x, 0.5f + el0 / dV - 0.5f * eq1.scale.y};
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&eq1;
            // M (4x MSAA), F (FP16), T (10-bit), S (colour scale / bias): opaque 0.2 x 0.1 m quads, LOCAL, z = -2
            auto quad = [&](XrCompositionLayerQuad& q, int k, float x, float y) {
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = space; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH; sub(q.subImage, k);
                q.pose = {{0, 0, 0, 1}, {x, y, -2.0f}}; q.size = {0.2f, 0.1f};
                layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&q;
            };
            quad(qm, 10, 1.45f, 2.40f);
            quad(qf, 11, 1.45f, 2.057f);
            quad(qt, 12, 1.45f, 1.714f);
            // S: white, colour = white * (0.5, 0.25, 0, 1) + (0, 0, 0.2, 0) in linear light
            csb.colorScale = {0.5f, 0.25f, 0.0f, 1.0f};
            csb.colorBias = {0.0f, 0.0f, 0.2f, 0.0f};
            qs.next = &csb;
            quad(qs, 13, 1.02f, 2.057f);
        }
        XrCompositionLayerQuad hv{XR_TYPE_COMPOSITION_LAYER_QUAD}, hw{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (hud) {
            for (int k = 0; k < 2; ++k) {
                XrCompositionLayerQuad& q = k == 0 ? hv : hw;
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = k == 0 ? viewSpace : space;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = huds[k].h;
                q.subImage.imageRect = {{0, 0}, {64, 32}};
                layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&q;
            }
            hv.pose = {{0, 0, 0, 1}, {0.12f, -0.10f, -1.0f}}; hv.size = {0.10f, 0.06f};
            hw.pose = {{0, 0, 0, 1}, {-0.25f, 1.75f, -2.0f}}; hw.size = {0.12f, 0.08f};
        }
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = nLayers;
        fei.layers = layers;
        CHECK(xrEndFrame(session, &fei));
    }

    // ---- teardown (the layer finalizes in xrDestroySession if still recording) ----
    if (d3d12) { queue->Signal(fence.Get(), ++fenceVal); fence->SetEventOnCompletion(fenceVal, evt); WaitForSingleObject(evt, 5000); }
    CHECK(xrEndSession(session));
    for (auto& s : scs) CHECK(xrDestroySwapchain(s.h));
    for (auto& s : ovs) CHECK(xrDestroySwapchain(s.h));
    for (auto& s : huds) CHECK(xrDestroySwapchain(s.h));
    if (viewSpace) xrDestroySpace(viewSpace);
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
