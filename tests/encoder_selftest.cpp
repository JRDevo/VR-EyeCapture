// encoder_selftest -- drives the layer's own capture pipeline (IFrameSource + Recorder + audio + st3d) without a
// headset: a "game" device (D3D11 or D3D12, created like a game would, WITHOUT video support) draws the known test
// pattern (tests/test_pattern.h) into "swapchain" textures laid out like an OpenXR app would submit them, and every
// frame goes through exactly the call the layer makes in xrEndFrame (Recorder::SubmitFrame).
//
//   encoder_selftest [--api d3d11|d3d12] [--layout two|array|wide|msaa] [--eye 1920x1080] [--seconds 5] [--fps 90]
//                    [--format srgb|unorm] [--codec hevc|h264] [--out file.mp4] [--no-audio] [--no-tone]
// Exit code 0 = file written with >= 90 % of the frames.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mmsystem.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>
#include "frame_source.h"
#include "log.h"
#include "recorder.h"
#include "test_pattern.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    std::string api = "d3d11", layout = "two", format = "srgb", codec = "hevc";
    UINT eyeW = 1920, eyeH = 1080, fps = 90;
    double seconds = 5;
    std::wstring out;
    bool audio = true, tone = true;
};

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

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

// One "swapchain" of the simulated app.
struct FakeSwapchain {
    SwapchainInfo info;
    ComPtr<ID3D11Texture2D> t11;
    std::vector<ComPtr<ID3D11RenderTargetView>> rtv11;   // per array slice
    ComPtr<ID3D12Resource> t12;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtv12;
};

} // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (k == "--api") a.api = next();
        else if (k == "--layout") a.layout = next();
        else if (k == "--format") a.format = next();
        else if (k == "--codec") a.codec = next();
        else if (k == "--seconds") a.seconds = atof(next().c_str());
        else if (k == "--fps") a.fps = (UINT)atoi(next().c_str());
        else if (k == "--eye") { std::string v = next(); sscanf_s(v.c_str(), "%ux%u", &a.eyeW, &a.eyeH); }
        else if (k == "--out") a.out = elog::Widen(next());
        else if (k == "--no-audio") a.audio = false;
        else if (k == "--no-tone") a.tone = false;
        else { printf("unknown argument %s\n", k.c_str()); return 2; }
    }
    const bool d3d12 = a.api == "d3d12";
    const std::wstring dir = ExeDir();
    elog::Init(dir + L"\\encoder_selftest.log", true);
    elog::SetLevel(LogLevel::Debug);
    if (a.out.empty()) a.out = dir + L"\\selftest_" + elog::Widen(a.api + "_" + a.layout + "_" + a.format + "_" + a.codec) + L".mp4";
    LOGI("=== encoder_selftest: api=%s layout=%s eye=%ux%u format=%s codec=%s %.1f s @ %u fps -> %s", a.api.c_str(),
         a.layout.c_str(), a.eyeW, a.eyeH, a.format.c_str(), a.codec.c_str(), a.seconds, a.fps, elog::Narrow(a.out).c_str());
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IDXGIAdapter1> adapter = PickAdapter();
    if (!adapter) { LOGE("no hardware adapter"); return 1; }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    LOGI("game adapter: %s", elog::Narrow(ad.Description).c_str());

    const bool srgb = a.format == "srgb";
    const DXGI_FORMAT typedFmt = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    const DXGI_FORMAT texFmt = DXGI_FORMAT_B8G8R8A8_TYPELESS;   // what runtimes typically allocate
    const DXGI_FORMAT rtvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;      // write exact bytes
    const UINT w = a.eyeW, h = a.eyeH;
    const bool msaa = a.layout == "msaa", arr = a.layout == "array", wide = a.layout == "wide";
    const UINT samples = msaa ? 4 : 1;

    // ---- the simulated swapchains ----
    std::vector<FakeSwapchain> sc(arr || wide ? 1 : 2);
    for (auto& s : sc) {
        s.info.format = typedFmt;
        s.info.width = wide ? 2 * w : w;
        s.info.height = h;
        s.info.arraySize = arr ? 2 : 1;
        s.info.mipCount = 1;
        s.info.sampleCount = samples;
        s.info.usageFlags = 0x1 | 0x10 | 0x20;   // colour attachment | transfer src | sampled
    }

    // ---- game device ----
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11DeviceContext1> ctx11_1;
    ComPtr<ID3D12Device> dev12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Fence> fence12;
    std::vector<ComPtr<ID3D12CommandAllocator>> alloc12(3);
    std::vector<uint64_t> allocFence(3, 0);
    ComPtr<ID3D12GraphicsCommandList> list12;
    uint64_t fenceVal = 0;
    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::unique_ptr<IFrameSource> src;

    if (!d3d12) {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                       D3D11_SDK_VERSION, &dev11, nullptr, &ctx11);
        if (FAILED(hr)) { LOGE("game D3D11 device failed hr=0x%08lx", (unsigned long)hr); return 1; }
        ctx11.As(&ctx11_1);
        for (auto& s : sc) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = s.info.width; d.Height = h; d.MipLevels = 1; d.ArraySize = s.info.arraySize; d.Format = texFmt;
            d.SampleDesc.Count = samples; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(hr = dev11->CreateTexture2D(&d, nullptr, &s.t11))) { LOGE("swapchain texture failed hr=0x%08lx", (unsigned long)hr); return 1; }
            for (UINT sl = 0; sl < s.info.arraySize; ++sl) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = rtvFmt;
                if (samples > 1) { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY; rd.Texture2DMSArray.FirstArraySlice = sl; rd.Texture2DMSArray.ArraySize = 1; }
                else { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; rd.Texture2DArray.FirstArraySlice = sl; rd.Texture2DArray.ArraySize = 1; }
                ComPtr<ID3D11RenderTargetView> v;
                if (FAILED(hr = dev11->CreateRenderTargetView(s.t11.Get(), &rd, &v))) { LOGE("RTV failed hr=0x%08lx", (unsigned long)hr); return 1; }
                s.rtv11.push_back(v);
            }
        }
        src = CreateD3D11FrameSource(dev11.Get());
    } else {
        HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12));
        if (FAILED(hr)) { LOGE("game D3D12 device failed hr=0x%08lx", (unsigned long)hr); return 1; }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12));
        dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12));
        for (auto& al : alloc12) dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
        dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc12[0].Get(), nullptr, IID_PPV_ARGS(&list12));
        list12->Close();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 8;
        dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
        const UINT inc = dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (auto& s : sc) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = s.info.width; rd.Height = h; rd.DepthOrArraySize = (UINT16)s.info.arraySize; rd.MipLevels = 1;
            rd.Format = texFmt; rd.SampleDesc.Count = samples; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (FAILED(hr = dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                           nullptr, IID_PPV_ARGS(&s.t12)))) {
                LOGE("D3D12 swapchain texture failed hr=0x%08lx", (unsigned long)hr);
                return 1;
            }
            for (UINT sl = 0; sl < s.info.arraySize; ++sl) {
                D3D12_RENDER_TARGET_VIEW_DESC v{};
                v.Format = rtvFmt;
                if (samples > 1) { v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY; v.Texture2DMSArray.FirstArraySlice = sl; v.Texture2DMSArray.ArraySize = 1; }
                else { v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray.FirstArraySlice = sl; v.Texture2DArray.ArraySize = 1; }
                dev12->CreateRenderTargetView(s.t12.Get(), &v, hnd);
                s.rtv12.push_back(hnd);
                hnd.ptr += inc;
            }
        }
        src = CreateD3D12FrameSource(dev12.Get(), queue12.Get());
    }
    if (!src) { LOGE("frame source creation failed"); return 1; }

    // ---- eye views, exactly as the layer builds them from XrCompositionLayerProjectionView ----
    EyeView eyes[2];
    for (int e = 0; e < 2; ++e) {
        FakeSwapchain& s = sc[(arr || wide) ? 0 : (size_t)e];
        eyes[e].image = d3d12 ? (void*)s.t12.Get() : (void*)s.t11.Get();
        eyes[e].sc = &s.info;
        eyes[e].swapchainKey = &s;
        eyes[e].arrayIndex = arr ? (uint32_t)e : 0;
        eyes[e].x = wide ? e * (int)w : 0;
        eyes[e].y = 0;
        eyes[e].w = w;
        eyes[e].h = h;
    }

    DXGI_FORMAT stage{};
    bool srgbData = false;
    if (!src->StageFormat(sc[0].info, &stage, &srgbData)) { LOGE("unsupported format"); return 1; }

    RecorderSettings rs;
    rs.path = a.out;
    rs.hevc = a.codec != "h264";
    rs.fps = a.fps;
    rs.audio = a.audio;
    Recorder rec;
    rec.Start(src.get(), rs, 2 * w, h, w, stage, srgbData);
    {
        const ULONGLONG t0 = GetTickCount64();
        while (rec.State() == RecState::Starting && GetTickCount64() - t0 < 15000) Sleep(5);
    }
    if (rec.State() != RecState::Recording) {
        rec.WaitIdle(10000);
        LOGE("recorder did not start: %s", rec.Status().message.c_str());
        return 1;
    }

    std::vector<uint8_t> wav;
    if (a.audio && a.tone) {
        wav = MakeToneWav(a.seconds + 1.0, 440.0, 0.2);
        PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }

    // ---- frame loop ----
    timeBeginPeriod(1);
    const UINT frames = (UINT)(a.seconds * a.fps + 0.5);
    const int64_t period = 10000000LL / a.fps;
    const int64_t base = QpcNow100ns() + 50000;
    std::vector<PatRect> pat;
    UINT submitted = 0;
    double cpuMax = 0, cpuSum = 0;
    for (UINT f = 0; f < frames; ++f) {
        const int64_t due = base + (int64_t)f * period;
        for (int64_t now = QpcNow100ns(); now < due; now = QpcNow100ns()) {
            if (due - now > 20000) Sleep(1);
        }
        // draw both eyes
        if (!d3d12) {
            for (int e = 0; e < 2; ++e) {
                FakeSwapchain& s = sc[(arr || wide) ? 0 : (size_t)e];
                ID3D11RenderTargetView* rtv = s.rtv11[arr ? (size_t)e : 0].Get();
                BuildPattern(pat, e, f, eyes[e].x, 0, (int)w, (int)h);
                for (auto& p : pat) ctx11_1->ClearView(rtv, p.c, &p.r, 1);
            }
        } else {
            const UINT k = f % 3;
            if (fence12->GetCompletedValue() < allocFence[k]) {
                fence12->SetEventOnCompletion(allocFence[k], fenceEvt);
                WaitForSingleObject(fenceEvt, 5000);
            }
            alloc12[k]->Reset();
            list12->Reset(alloc12[k].Get(), nullptr);
            for (int e = 0; e < 2; ++e) {
                FakeSwapchain& s = sc[(arr || wide) ? 0 : (size_t)e];
                BuildPattern(pat, e, f, eyes[e].x, 0, (int)w, (int)h);
                for (auto& p : pat) list12->ClearRenderTargetView(s.rtv12[arr ? (size_t)e : 0], p.c, 1, &p.r);
            }
            list12->Close();
            ID3D12CommandList* l[] = {list12.Get()};
            queue12->ExecuteCommandLists(1, l);
            queue12->Signal(fence12.Get(), ++fenceVal);
            allocFence[k] = fenceVal;
        }
        // the layer's xrEndFrame work
        const int64_t c0 = QpcNow100ns();
        if (rec.SubmitFrame(due, eyes[0], eyes[1])) ++submitted;
        const double ms = (QpcNow100ns() - c0) / 1e4;
        cpuSum += ms;
        if (ms > cpuMax) cpuMax = ms;
        if (!d3d12) ctx11->Flush();
    }
    timeEndPeriod(1);
    LOGI("frame loop done: %u frames, %u submitted, game-thread SubmitFrame cost avg %.3f ms max %.3f ms", frames, submitted,
         cpuSum / frames, cpuMax);

    rec.Stop();
    const bool idle = rec.WaitIdle(60000);
    PlaySoundW(nullptr, nullptr, 0);
    RecorderStatus st = rec.Status();
    LOGI("result: %s state=%s written=%llu dropped=%llu elapsed=%.2f s msg='%s'", idle ? "finished" : "TIMEOUT",
         RecStateName(st.state), (unsigned long long)st.framesWritten, (unsigned long long)st.framesDropped, st.elapsedSec,
         st.message.c_str());
    if (d3d12) {
        queue12->Signal(fence12.Get(), ++fenceVal);
        fence12->SetEventOnCompletion(fenceVal, fenceEvt);
        WaitForSingleObject(fenceEvt, 5000);
    }
    CloseHandle(fenceEvt);
    const bool ok = idle && st.framesWritten >= frames * 9 / 10 && GetFileAttributesW(a.out.c_str()) != INVALID_FILE_ATTRIBUTES;
    LOGI("=== encoder_selftest %s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
