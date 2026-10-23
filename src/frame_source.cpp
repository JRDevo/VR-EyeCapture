// frame_source -- shared helpers (format mapping, encoder device creation). See frame_source.h.
#include "frame_source.h"
#include <dxgi1_6.h>
#include <string>
#include "log.h"

const char* GfxApiName(GfxApi api) {
    switch (api) {
    case GfxApi::None:   return "none";
    case GfxApi::D3D11:  return "D3D11";
    case GfxApi::D3D12:  return "D3D12";
    case GfxApi::Vulkan: return "Vulkan";
    case GfxApi::OpenGL: return "OpenGL";
    default:             return "unknown";
    }
}

DXGI_FORMAT StageFormatForDxgi(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsSrgbDxgi(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

const char* DxgiFormatName(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:      return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:         return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:    return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:      return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM:         return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:      return "B8G8R8X8_TYPELESS";
    case DXGI_FORMAT_B8G8R8X8_UNORM:         return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:    return "B8G8R8X8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:   return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM:      return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:  return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:     return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT:        return "R11G11B10_FLOAT";
    case DXGI_FORMAT_D32_FLOAT:              return "D32_FLOAT";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:      return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:   return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_D16_UNORM:              return "D16_UNORM";
    case DXGI_FORMAT_NV12:                   return "NV12";
    case DXGI_FORMAT_P010:                   return "P010";
    default:                                 return "other";
    }
}

int64_t QpcNow100ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // split to avoid overflow: whole seconds + remainder
    const int64_t sec = c.QuadPart / freq.QuadPart;
    const int64_t rem = c.QuadPart % freq.QuadPart;
    return sec * 10000000LL + rem * 10000000LL / freq.QuadPart;
}

ComPtr<ID3D11Device> CreateEncoderDevice(LUID luid, std::string* adapterName) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        LOGE("encoder device: CreateDXGIFactory1 failed");
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        LOGE("encoder device: no adapter with the game's LUID %08lx:%08lx (hr=0x%08lx)", (unsigned long)luid.HighPart,
             (unsigned long)luid.LowPart, (unsigned long)hr);
        return nullptr;
    }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    if (adapterName) *adapterName = elog::Narrow(ad.Description);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1};
    ComPtr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL got{};
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION, &dev, &got, nullptr);
    if (FAILED(hr)) {
        LOGE("encoder device: D3D11CreateDevice(VIDEO_SUPPORT) on '%s' failed hr=0x%08lx",
             elog::Narrow(ad.Description).c_str(), (unsigned long)hr);
        return nullptr;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);   // Media Foundation requires it
    ComPtr<ID3D11Device5> dev5;
    if (FAILED(dev.As(&dev5))) {
        LOGE("encoder device: ID3D11Device5 not available (shared fences need Windows 10 1703+ / WDDM 2.2)");
        return nullptr;
    }
    LOGI("encoder device: private D3D11 device on '%s' (feature level %x, video support)",
         elog::Narrow(ad.Description).c_str(), (unsigned)got);
    return dev;
}
