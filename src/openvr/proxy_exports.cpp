// proxy_exports -- the exports of the SteamVR proxy openvr_api.dll (VR EyeCapture). The game loads this DLL instead of
// Valve's, which the tray's Enable renamed to openvr_api_real.dll next to it. Every export of the real DLL exists here
// (openvr_api.def, from `dumpbin /exports` of Valve's openvr_api.dll 1.13.10) and is an explicit trampoline to the real
// one -- resolved with GetProcAddress at the first call, never in DllMain, so a missing real DLL is logged instead of
// failing the game's load. Hooked: VR_InitInternal / VR_InitInternal2 (arm check), VR_ShutdownInternal (finalize,
// unpatch), VR_GetGenericInterface (the IVRCompositor hooks, openvr_capture.cpp). The interface accessors of unknown
// signature (VRHeadsetView, VRPaths, LiquidVR, ...) are register-preserving jump stubs (accessor_stubs.asm).
#include <windows.h>
#include <string>
#include "../../third_party/openvr/openvr.h"
#include "openvr_capture.h"
#include "openvr_hooks.h"

namespace {

struct RealApi {
    HMODULE mod = nullptr;
    PFN_VR_InitInternal InitInternal = nullptr;
    PFN_VR_InitInternal2 InitInternal2 = nullptr;
    PFN_VR_ShutdownInternal ShutdownInternal = nullptr;
    PFN_VR_GetGenericInterface GetGenericInterface = nullptr;
    PFN_VR_IsInterfaceVersionValid IsInterfaceVersionValid = nullptr;
    PFN_VR_GetInitToken GetInitToken = nullptr;
    PFN_VR_IsHmdPresent IsHmdPresent = nullptr;
    PFN_VR_IsRuntimeInstalled IsRuntimeInstalled = nullptr;
    PFN_VR_RuntimePath RuntimePath = nullptr;
    PFN_VR_GetRuntimePath GetRuntimePath = nullptr;
    PFN_VR_GetVRInitErrorAsSymbol GetVRInitErrorAsSymbol = nullptr;
    PFN_VR_GetVRInitErrorAsEnglishDescription GetVRInitErrorAsEnglishDescription = nullptr;
    PFN_VR_GetStringForHmdError GetStringForHmdError = nullptr;
};
RealApi g_real;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

// Same order as the stubs in accessor_stubs.asm.
const char* const kAccessorNames[] = {
    "LiquidVR", "VRCompositorSystemInternal", "VRControlPanel", "VRDriverDirectInternal", "VRHeadsetView",
    "VRPaths", "VRRenderModelsInternal", "VRSceneGraph", "VRTrackedCameraInternal", "VRVirtualDisplay",
};
const int kAccessorCount = (int)(sizeof(kAccessorNames) / sizeof(kAccessorNames[0]));

std::wstring ProxyDir() {
    HMODULE hm = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ProxyDir, &hm);
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(hm, p, MAX_PATH);
    std::wstring s = p;
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

} // namespace

extern "C" void* g_eyecapAccessors[10] = {};   // read by accessor_stubs.asm (0 = not resolved yet)

namespace {

BOOL CALLBACK InitOnce(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring dir = ProxyDir();
    const std::wstring realPath = dir + L"\\" EYECAP_PROXY_REAL_DLL;
    // LOAD_WITH_ALTERED_SEARCH_PATH: the real DLL's own dependencies are searched next to it first
    g_real.mod = LoadLibraryExW(realPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD err = g_real.mod ? 0 : GetLastError();
    if (g_real.mod) {
#define R(fn) g_real.fn = (decltype(g_real.fn))GetProcAddress(g_real.mod, "VR_" #fn)
        R(InitInternal); R(InitInternal2); R(ShutdownInternal); R(GetGenericInterface); R(IsInterfaceVersionValid);
        R(GetInitToken); R(IsHmdPresent); R(IsRuntimeInstalled); R(RuntimePath); R(GetRuntimePath);
        R(GetVRInitErrorAsSymbol); R(GetVRInitErrorAsEnglishDescription); R(GetStringForHmdError);
#undef R
        for (int i = 0; i < kAccessorCount; ++i) g_eyecapAccessors[i] = (void*)GetProcAddress(g_real.mod, kAccessorNames[i]);
    }
    OvrProcessInit(dir, g_real.mod != nullptr, realPath, err);
    return TRUE;
}

void Ensure() { InitOnceExecuteOnce(&g_once, InitOnce, nullptr, nullptr); }

template <class F> F Need(F f, const char* name) {
    if (!f) OvrLogMissingExport(name);
    return f;
}

} // namespace

// Called by an accessor stub whose pointer is still 0: resolve everything, return the real accessor (0 = missing).
extern "C" void* EyeCapResolveAccessor(unsigned idx) {
    Ensure();
    if (idx >= (unsigned)kAccessorCount) return nullptr;
    if (!g_eyecapAccessors[idx]) OvrLogMissingExport(kAccessorNames[idx]);
    return g_eyecapAccessors[idx];
}

extern "C" {

uint32_t VR_InitInternal2(vr::EVRInitError* peError, vr::EVRApplicationType eType, const char* pStartupInfo) {
    Ensure();
    auto f = Need(g_real.InitInternal2, "VR_InitInternal2");
    if (!f) {
        if (peError) *peError = vr::VRInitError_Init_InstallationNotFound;
        return 0;
    }
    vr::EVRInitError local = vr::VRInitError_None;
    vr::EVRInitError* pe = peError ? peError : &local;
    const uint32_t token = f(pe, eType, pStartupInfo);
    OvrOnInit("VR_InitInternal2", eType, *pe, token);
    return token;
}

uint32_t VR_InitInternal(vr::EVRInitError* peError, vr::EVRApplicationType eType) {
    Ensure();
    auto f = Need(g_real.InitInternal, "VR_InitInternal");
    if (!f) {
        if (peError) *peError = vr::VRInitError_Init_InstallationNotFound;
        return 0;
    }
    vr::EVRInitError local = vr::VRInitError_None;
    vr::EVRInitError* pe = peError ? peError : &local;
    const uint32_t token = f(pe, eType);
    OvrOnInit("VR_InitInternal", eType, *pe, token);
    return token;
}

void VR_ShutdownInternal() {
    Ensure();
    OvrOnShutdown();   // finalize + unpatch before vrclient goes away
    if (auto f = Need(g_real.ShutdownInternal, "VR_ShutdownInternal")) f();
}

void* VR_GetGenericInterface(const char* pchInterfaceVersion, vr::EVRInitError* peError) {
    Ensure();
    auto f = Need(g_real.GetGenericInterface, "VR_GetGenericInterface");
    if (!f) {
        if (peError) *peError = vr::VRInitError_Init_InstallationNotFound;
        return nullptr;
    }
    void* p = f(pchInterfaceVersion, peError);
    if (!p || !OvrActive()) return p;
    return OvrOnInterface(pchInterfaceVersion, p, f);
}

bool VR_IsInterfaceVersionValid(const char* pchInterfaceVersion) {
    Ensure();
    auto f = Need(g_real.IsInterfaceVersionValid, "VR_IsInterfaceVersionValid");
    return f ? f(pchInterfaceVersion) : false;
}

uint32_t VR_GetInitToken() {
    Ensure();
    auto f = Need(g_real.GetInitToken, "VR_GetInitToken");
    return f ? f() : 0;
}

bool VR_IsHmdPresent() {
    Ensure();
    auto f = Need(g_real.IsHmdPresent, "VR_IsHmdPresent");
    return f ? f() : false;
}

bool VR_IsRuntimeInstalled() {
    Ensure();
    auto f = Need(g_real.IsRuntimeInstalled, "VR_IsRuntimeInstalled");
    return f ? f() : false;
}

const char* VR_RuntimePath() {
    Ensure();
    auto f = Need(g_real.RuntimePath, "VR_RuntimePath");
    return f ? f() : nullptr;
}

bool VR_GetRuntimePath(char* pchPathBuffer, uint32_t unBufferSize, uint32_t* punRequiredBufferSize) {
    Ensure();
    auto f = Need(g_real.GetRuntimePath, "VR_GetRuntimePath");
    if (!f) {
        if (punRequiredBufferSize) *punRequiredBufferSize = 0;
        return false;
    }
    return f(pchPathBuffer, unBufferSize, punRequiredBufferSize);
}

const char* VR_GetVRInitErrorAsSymbol(vr::EVRInitError error) {
    Ensure();
    auto f = Need(g_real.GetVRInitErrorAsSymbol, "VR_GetVRInitErrorAsSymbol");
    return f ? f(error) : "VRInitError_Init_InstallationNotFound";
}

const char* VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError error) {
    Ensure();
    auto f = Need(g_real.GetVRInitErrorAsEnglishDescription, "VR_GetVRInitErrorAsEnglishDescription");
    return f ? f(error) : "VR EyeCapture proxy: openvr_api_real.dll is missing next to openvr_api.dll";
}

const char* VR_GetStringForHmdError(vr::EVRInitError error) {
    Ensure();
    auto f = Need(g_real.GetStringForHmdError, "VR_GetStringForHmdError");
    return f ? f(error) : "VR EyeCapture proxy: openvr_api_real.dll is missing next to openvr_api.dll";
}

// Ours: its presence marks this openvr_api.dll as the VR EyeCapture proxy (the tray reads the export table; the version
// is found in the file as "EYECAP_PROXY_VERSION=<x>", the string this function returns the tail of).
const char kProxyVersionMarker[] = "EYECAP_PROXY_VERSION=" EYE_CAPTURE_VERSION;
const char* EyeCaptureProxyVersion() { return kProxyVersionMarker + 21; }

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;   // nothing here (loader lock): the real DLL is loaded at the first export call
}
