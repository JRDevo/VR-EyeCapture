// mock_openvr -- a stand-in for Valve's openvr_api.dll (no SteamVR, no headset) for testing the VR EyeCapture SteamVR
// proxy. tests/run_steamvr.ps1 copies it next to the proxy as openvr_api_real.dll. It has every export of Valve's DLL
// 1.13.10 (same names) plus MockOpenVrCounts (test only).
//
//   IVRCompositor_028 / _022 (and _099, a "future" version the proxy does not know: the _028 object): C++ objects whose
//   vtables are full-size, READ-ONLY tables (like vrclient's) -- WaitGetPoses (slot 2) and Submit (slot 5) are real, every
//   other slot a counting stub; "FnTable:IVRCompositor_0xx": function tables (no this) with the same two real entries.
//   IVRSystem_022: a real C++ class (vr::IVRSystem of third_party/openvr/openvr.h -- proves the proxy's raw slot calls
//   match the compiler's virtual-call ABI); IVRSystem_019: a raw vtable with ResetSeatedZeroPose at 12 (float property 23).
//   GetProjectionRaw: left eye l -1.1 r 0.9 t -1.0 b 0.9, right eye mirrored (l -0.9 r 1.1); GetEyeToHeadTransform: -+0.032 m
//   on x; display 90 Hz.
//   WaitGetPoses: render pose [0] = the HMD on the head motion of tests/test_motion.h at (0, 1.6, 0), t = seconds since the
//   first call; MOCK_OPENVR_NO_POSE=1 reports it invalid.
//   Submit: validates the handle (an ID3D11Texture2D), type, bounds (0..1, min < max unless flipped on purpose), flags and the
//   pose struct (TextureWithPose: a finite rotation); counts per eye and returns VRCompositorError_None, or
//   MOCK_OPENVR_SUBMIT_ERROR=<n>.
//   MOCK_OPENVR_SYSTEM=019: only IVRSystem_019 exists (the proxy must fall back to it).
//   Mirror recorder (EyeCaptureMirror.exe, quick mode): "FnTable:IVRCompositor_022" slot 33 GetMirrorTextureD3D11 returns an SRV
//   (on the caller's device) of a 1440x1600 R8G8B8A8_UNORM_SRGB texture per eye with tests/test_pattern.h drawn into it (bar at
//   frame 0); MOCK_OPENVR_MIRROR_COMBINED=1: one 2880x1600 texture for both eyes (left | right), the same SRV for both eyes.
//   Slot 34 ReleaseMirrorTextureD3D11 releases it; both are counted. Slot 22 GetCurrentSceneFocusProcess = MOCK_OPENVR_SCENE_PID
//   (0 if unset). IVRSystem_022 PollNextEvent (slot 29) returns VREvent_Quit once MOCK_OPENVR_QUIT_AFTER_MS ms have passed since
//   VR_InitInternal2 (only with the 64-byte VREvent_t). VR_InitInternal2(VRApplication_Background) fails with
//   VRInitError_Init_NoServerForBackgroundApp while MOCK_OPENVR_NO_SERVER=1.
// At VR_ShutdownInternal it prints "mock: submits L n R n, waitgetposes n, fntable submits n, errors n, stub calls n,
// accessor calls n, interfaces asked: ...".
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include "../third_party/openvr/openvr.h"
#include "test_motion.h"
#include "test_pattern.h"
#include <vector>

#define MOCK_EXPORT extern "C" __declspec(dllexport)

namespace {

std::atomic<int64_t> g_mirrorGets{0}, g_mirrorReleases{0}, g_events{0}, g_backgroundInits{0};
int64_t g_initTick = 0;
std::atomic<int64_t> g_submitL{0}, g_submitR{0}, g_wait{0}, g_fnSubmits{0}, g_invalid{0}, g_stubCalls{0}, g_accessorCalls{0},
    g_shutdowns{0}, g_systemCalls{0};
std::mutex g_m;
std::string g_asked;
std::string g_firstInvalid;
uint32_t g_token = 0;
bool g_inited = false;
int64_t g_t0 = 0;

std::string Env(const char* n) {
    char b[256];
    const DWORD k = GetEnvironmentVariableA(n, b, sizeof(b));
    return (k > 0 && k < sizeof(b)) ? std::string(b) : std::string();
}

void Invalid(const char* why) {
    ++g_invalid;
    std::lock_guard<std::mutex> lk(g_m);
    if (g_firstInvalid.empty()) g_firstInvalid = why;
}

double Seconds() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void HeadMatrix(double t, vr::HmdMatrix34_t& m) {
    float r[9];
    test_motion::Matrix(test_motion::Head(t), r);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m.m[i][j] = r[i * 3 + j];
    m.m[0][3] = 0; m.m[1][3] = 1.6f; m.m[2][3] = 0;
}

bool FiniteRotation(const vr::HmdMatrix34_t& m) {
    for (int i = 0; i < 3; ++i) {
        double n = 0;
        for (int j = 0; j < 4; ++j) if (!std::isfinite(m.m[i][j])) return false;
        for (int j = 0; j < 3; ++j) n += (double)m.m[i][j] * m.m[i][j];
        if (std::fabs(n - 1.0) > 1e-3) return false;
    }
    return true;
}

vr::EVRCompositorError DoWaitGetPoses(vr::TrackedDevicePose_t* render, uint32_t nRender, vr::TrackedDevicePose_t* game, uint32_t nGame) {
    ++g_wait;
    if (!g_t0) { LARGE_INTEGER c; QueryPerformanceCounter(&c); g_t0 = c.QuadPart; }
    static const double t0 = Seconds();
    const double t = Seconds() - t0;
    const bool noPose = Env("MOCK_OPENVR_NO_POSE") == "1";
    for (uint32_t i = 0; render && i < nRender; ++i) {
        vr::TrackedDevicePose_t& p = render[i];
        memset(&p, 0, sizeof(p));
        if (i == 0) {
            HeadMatrix(t, p.mDeviceToAbsoluteTracking);
            p.eTrackingResult = vr::TrackingResult_Running_OK;
            p.bPoseIsValid = !noPose;
            p.bDeviceIsConnected = true;
        }
    }
    for (uint32_t i = 0; game && i < nGame; ++i) memset(&game[i], 0, sizeof(game[i]));
    Sleep(1);   // a little like the real call, which blocks until the compositor wants the next frame
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError DoSubmit(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* b, int flags) {
    if (eye == vr::Eye_Left) ++g_submitL; else if (eye == vr::Eye_Right) ++g_submitR; else Invalid("bad eye");
    if (!tex || !tex->handle) Invalid("null texture");
    else if (tex->eType != vr::TextureType_DirectX) Invalid("texture type");
    else {
        ID3D11Texture2D* t = nullptr;
        if (FAILED(static_cast<IUnknown*>(tex->handle)->QueryInterface(IID_PPV_ARGS(&t))) || !t) Invalid("handle is not an ID3D11Texture2D");
        else t->Release();
        if (tex->eColorSpace < vr::ColorSpace_Auto || tex->eColorSpace > vr::ColorSpace_Linear) Invalid("colour space");
    }
    if (b) {
        const float v[4] = {b->uMin, b->vMin, b->uMax, b->vMax};
        for (float x : v) if (!(x >= 0 && x <= 1)) Invalid("bounds outside 0..1");
        if (b->uMin == b->uMax || b->vMin == b->vMax) Invalid("empty bounds");
    }
    const int known = vr::Submit_LensDistortionAlreadyApplied | vr::Submit_TextureWithPose | vr::Submit_TextureWithDepth |
                      vr::Submit_FrameDiscontinuty;
    if (flags & ~known) Invalid("unknown submit flags");
    if ((flags & vr::Submit_TextureWithPose) && tex &&
        !FiniteRotation(static_cast<const vr::VRTextureWithPose_t*>(tex)->mDeviceToAbsoluteTracking))
        Invalid("TextureWithPose pose is not a rotation");
    const std::string e = Env("MOCK_OPENVR_SUBMIT_ERROR");
    return e.empty() ? vr::VRCompositorError_None : (vr::EVRCompositorError)atoi(e.c_str());
}

// ---------------------------------------------------------------- IVRCompositor: C++ objects with raw read-only vtables

template <int N> intptr_t CppStub(void*) { ++g_stubCalls; return 0; }
vr::EVRCompositorError CppWait(void*, vr::TrackedDevicePose_t* r, uint32_t nr, vr::TrackedDevicePose_t* gp, uint32_t ng) {
    return DoWaitGetPoses(r, nr, gp, ng);
}
vr::EVRCompositorError CppSubmit(void*, vr::EVREye eye, const vr::Texture_t* t, const vr::VRTextureBounds_t* b, vr::EVRSubmitFlags f) {
    return DoSubmit(eye, t, b, (int)f);
}

#define S(n) (void*)&CppStub<n>
void* const kVt028[52] = {
    S(0), S(1), (void*)&CppWait, S(3), S(4), (void*)&CppSubmit, S(6), S(7), S(8), S(9),
    S(10), S(11), S(12), S(13), S(14), S(15), S(16), S(17), S(18), S(19),
    S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29),
    S(30), S(31), S(32), S(33), S(34), S(35), S(36), S(37), S(38), S(39),
    S(40), S(41), S(42), S(43), S(44), S(45), S(46), S(47), S(48), S(49),
    S(50), S(51)};
void* const kVt022[41] = {
    S(0), S(1), (void*)&CppWait, S(3), S(4), (void*)&CppSubmit, S(6), S(7), S(8), S(9),
    S(10), S(11), S(12), S(13), S(14), S(15), S(16), S(17), S(18), S(19),
    S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29),
    S(30), S(31), S(32), S(33), S(34), S(35), S(36), S(37), S(38), S(39),
    S(40)};
// _029: GetSubmitTexture inserted at 5 (a stub here), so Submit is slot 6
void* const kVt029[53] = {
    S(0), S(1), (void*)&CppWait, S(3), S(4), S(5), (void*)&CppSubmit, S(7), S(8), S(9),
    S(10), S(11), S(12), S(13), S(14), S(15), S(16), S(17), S(18), S(19),
    S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29),
    S(30), S(31), S(32), S(33), S(34), S(35), S(36), S(37), S(38), S(39),
    S(40), S(41), S(42), S(43), S(44), S(45), S(46), S(47), S(48), S(49),
    S(50), S(51), S(52)};
#undef S
struct RawObj { void* const* vtbl; };
RawObj g_comp029{kVt029};
RawObj g_comp028{kVt028};
RawObj g_comp022{kVt022};

// ---------------------------------------------------------------- IVRCompositor FnTables

template <int N> intptr_t FnStub() { ++g_stubCalls; return 0; }
vr::EVRCompositorError FnWait(vr::TrackedDevicePose_t* r, uint32_t nr, vr::TrackedDevicePose_t* gp, uint32_t ng) {
    return DoWaitGetPoses(r, nr, gp, ng);
}
vr::EVRCompositorError FnSubmit(vr::EVREye eye, vr::Texture_t* t, vr::VRTextureBounds_t* b, vr::EVRSubmitFlags f) {
    ++g_fnSubmits;
    return DoSubmit(eye, t, b, (int)f);
}
// mirror (quick mode): GetMirrorTextureD3D11 / ReleaseMirrorTextureD3D11 / GetCurrentSceneFocusProcess of FnTable _022
ID3D11ShaderResourceView* MakeMirrorSrv(ID3D11Device* dev, int eye, bool combined) {
    const UINT ew = 1440, eh = 1600, w = combined ? ew * 2 : ew;
    std::vector<uint32_t> px((size_t)w * eh);
    std::vector<PatRect> rects;
    for (int e = 0; e < (combined ? 2 : 1); ++e) {
        BuildPattern(rects, combined ? e : eye, 0, combined ? e * (int)ew : 0, 0, (int)ew, (int)eh);
        for (const PatRect& r : rects) {
            const uint32_t c = (uint32_t)std::lround(r.c[0] * 255) | ((uint32_t)std::lround(r.c[1] * 255) << 8) |
                               ((uint32_t)std::lround(r.c[2] * 255) << 16) | ((uint32_t)std::lround(r.c[3] * 255) << 24);
            for (LONG y = r.r.top; y < r.r.bottom && y < (LONG)eh; ++y)
                for (LONG x = r.r.left; x < r.r.right && x < (LONG)w; ++x) px[(size_t)y * w + x] = c;
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = eh; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (Env("MOCK_OPENVR_MIRROR_KEYEDMUTEX") == "1") d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;   // like a shared compositor texture
    D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
    ID3D11Texture2D* t = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, &init, &t))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(t, nullptr, &srv);
    t->Release();   // the SRV keeps it
    return srv;
}
vr::EVRCompositorError FnGetMirrorTextureD3D11(vr::EVREye eye, void* devOrRes, void** ppSrv) {
    if (!ppSrv || !devOrRes) { Invalid("GetMirrorTextureD3D11: null argument"); return vr::VRCompositorError_RequestFailed; }
    ID3D11Device* dev = nullptr;
    if (FAILED(static_cast<IUnknown*>(devOrRes)->QueryInterface(IID_PPV_ARGS(&dev))) || !dev) {
        Invalid("GetMirrorTextureD3D11: not an ID3D11Device");
        return vr::VRCompositorError_RequestFailed;
    }
    const bool combined = Env("MOCK_OPENVR_MIRROR_COMBINED") == "1";
    static ID3D11ShaderResourceView* s_combined = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (combined) {
        if (!s_combined) s_combined = MakeMirrorSrv(dev, 0, true);
        srv = s_combined;
        if (srv) srv->AddRef();   // every Get hands out a reference, released by ReleaseMirrorTextureD3D11
    } else {
        srv = MakeMirrorSrv(dev, eye == vr::Eye_Right ? 1 : 0, false);
    }
    dev->Release();
    if (!srv) return vr::VRCompositorError_RequestFailed;
    ++g_mirrorGets;
    *ppSrv = srv;
    return vr::VRCompositorError_None;
}
void FnReleaseMirrorTextureD3D11(void* srv) {
    if (!srv) { Invalid("ReleaseMirrorTextureD3D11(null)"); return; }
    ++g_mirrorReleases;
    static_cast<IUnknown*>(srv)->Release();
}
uint32_t FnGetCurrentSceneFocusProcess() {
    const std::string p = Env("MOCK_OPENVR_SCENE_PID");
    return p.empty() ? 0u : (uint32_t)strtoul(p.c_str(), nullptr, 10);
}

#define F(n) (void*)&FnStub<n>
void* g_fn028[52] = {
    F(0), F(1), (void*)&FnWait, F(3), F(4), (void*)&FnSubmit, F(6), F(7), F(8), F(9),
    F(10), F(11), F(12), F(13), F(14), F(15), F(16), F(17), F(18), F(19),
    F(20), F(21), F(22), F(23), F(24), F(25), F(26), F(27), F(28), F(29),
    F(30), F(31), F(32), F(33), F(34), F(35), F(36), F(37), F(38), F(39),
    F(40), F(41), F(42), F(43), F(44), F(45), F(46), F(47), F(48), F(49),
    F(50), F(51)};
void* g_fn029[53] = {
    F(0), F(1), (void*)&FnWait, F(3), F(4), F(5), (void*)&FnSubmit, F(7), F(8), F(9),
    F(10), F(11), F(12), F(13), F(14), F(15), F(16), F(17), F(18), F(19),
    F(20), F(21), F(22), F(23), F(24), F(25), F(26), F(27), F(28), F(29),
    F(30), F(31), F(32), F(33), F(34), F(35), F(36), F(37), F(38), F(39),
    F(40), F(41), F(42), F(43), F(44), F(45), F(46), F(47), F(48), F(49),
    F(50), F(51), F(52)};
void* g_fn022[41] = {
    F(0), F(1), (void*)&FnWait, F(3), F(4), (void*)&FnSubmit, F(6), F(7), F(8), F(9),
    F(10), F(11), F(12), F(13), F(14), F(15), F(16), F(17), F(18), F(19),
    F(20), F(21), (void*)&FnGetCurrentSceneFocusProcess, F(23), F(24), F(25), F(26), F(27), F(28), F(29),
    F(30), F(31), F(32), (void*)&FnGetMirrorTextureD3D11, (void*)&FnReleaseMirrorTextureD3D11, F(35), F(36), F(37), F(38), F(39),
    F(40)};
#undef F

// ---------------------------------------------------------------- IVRSystem

void ProjRaw(vr::EVREye eye, float* l, float* r, float* t, float* b) {
    ++g_systemCalls;
    if (eye == vr::Eye_Left) { *l = -1.1f; *r = 0.9f; } else { *l = -0.9f; *r = 1.1f; }
    *t = -1.0f;
    *b = 0.9f;
}
vr::HmdMatrix34_t EyeToHead(vr::EVREye eye) {
    ++g_systemCalls;
    vr::HmdMatrix34_t m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    m.m[0][3] = eye == vr::Eye_Left ? -0.032f : 0.032f;
    return m;
}

class MockSystem022 : public vr::IVRSystem {
public:
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override { if (w) *w = 1920; if (h) *h = 1080; }
    vr::HmdMatrix44_t GetProjectionMatrix(vr::EVREye, float, float) override { return vr::HmdMatrix44_t{}; }
    void GetProjectionRaw(vr::EVREye eye, float* l, float* r, float* t, float* b) override { ProjRaw(eye, l, r, t, b); }
    bool ComputeDistortion(vr::EVREye, float, float, vr::DistortionCoordinates_t*) override { return false; }
    vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eye) override { return EyeToHead(eye); }
    bool GetTimeSinceLastVsync(float*, uint64_t*) override { return false; }
    int32_t GetD3D9AdapterIndex() override { return 0; }
    void GetDXGIOutputInfo(int32_t* i) override { if (i) *i = 0; }
    void GetOutputDevice(uint64_t* d, vr::ETextureType, vr::VkInstance_T*) override { if (d) *d = 0; }
    bool IsDisplayOnDesktop() override { return false; }
    bool SetDisplayVisibility(bool) override { return false; }
    void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t*, uint32_t) override {}
    vr::HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override { return vr::HmdMatrix34_t{}; }
    vr::HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { return vr::HmdMatrix34_t{}; }
    uint32_t GetSortedTrackedDeviceIndicesOfClass(int, vr::TrackedDeviceIndex_t*, uint32_t, vr::TrackedDeviceIndex_t) override { return 0; }
    int GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t) override { return 0; }
    void ApplyTransform(vr::TrackedDevicePose_t*, const vr::TrackedDevicePose_t*, const vr::HmdMatrix34_t*) override {}
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(int) override { return 0; }
    int GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t) override { return 0; }
    int GetTrackedDeviceClass(vr::TrackedDeviceIndex_t) override { return 1; }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t) override { return true; }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) override {
        if (e) *e = vr::TrackedProp_WrongDataType;
        return false;
    }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t dev, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* e) override {
        ++g_systemCalls;
        if (dev == 0 && prop == vr::Prop_DisplayFrequency_Float) { if (e) *e = vr::TrackedProp_Success; return 90.0f; }
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return 0;
    }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) override {
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return 0;
    }
    uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) override {
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return 0;
    }
    vr::HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) override {
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return vr::HmdMatrix34_t{};
    }
    uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, uint32_t, void*, uint32_t,
                                           vr::ETrackedPropertyError* e) override {
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return 0;
    }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, char* v, uint32_t n,
                                            vr::ETrackedPropertyError* e) override {
        if (v && n) v[0] = 0;
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return 0;
    }
    const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return "TrackedProp_Mock"; }
    bool PollNextEvent(vr::VREvent_t* ev, uint32_t size) override {
        if (!ev || size != sizeof(vr::VREvent_t)) { Invalid("PollNextEvent: wrong VREvent_t size"); return false; }
        ++g_events;
        static bool s_quitSent = false;
        const std::string q = Env("MOCK_OPENVR_QUIT_AFTER_MS");
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        if (!q.empty() && !s_quitSent && g_initTick && (c.QuadPart - g_initTick) * 1000 / f.QuadPart >= atoll(q.c_str())) {
            s_quitSent = true;
            memset(ev, 0, sizeof(*ev));
            ev->eventType = vr::VREvent_Quit;
            return true;
        }
        return false;
    }
};
MockSystem022 g_sys022;

// IVRSystem_019 as a raw vtable (member-call ABI: this first, a struct result's hidden pointer second).
template <int N> intptr_t SysStub(void*) { ++g_stubCalls; return 0; }
void Sys019ProjRaw(void*, vr::EVREye eye, float* l, float* r, float* t, float* b) { ProjRaw(eye, l, r, t, b); }
vr::HmdMatrix34_t* Sys019EyeToHead(void*, vr::HmdMatrix34_t* out, vr::EVREye eye) { *out = EyeToHead(eye); return out; }
float Sys019Float(void*, vr::TrackedDeviceIndex_t dev, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* e) {
    ++g_systemCalls;
    if (dev == 0 && prop == vr::Prop_DisplayFrequency_Float) { if (e) *e = vr::TrackedProp_Success; return 90.0f; }
    if (e) *e = vr::TrackedProp_UnknownProperty;
    return 0;
}
float Sys019WrongType(void*, vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) {
    if (e) *e = vr::TrackedProp_WrongDataType;
    return 0;
}
#define Y(n) (void*)&SysStub<n>
void* const kSys019[30] = {
    Y(0), Y(1), (void*)&Sys019ProjRaw, Y(3), (void*)&Sys019EyeToHead, Y(5), Y(6), Y(7), Y(8), Y(9),
    Y(10), Y(11), Y(12) /* ResetSeatedZeroPose */, Y(13), Y(14), Y(15), Y(16), Y(17), Y(18), Y(19),
    Y(20), Y(21), (void*)&Sys019WrongType /* bool getter */, (void*)&Sys019Float, (void*)&Sys019WrongType /* int32 getter */,
    Y(25), Y(26), Y(27), Y(28), Y(29)};
#undef Y
RawObj g_sys019{kSys019};

int g_dummyAccessor[4];

// ---- IVROverlay (the proxy's in-headset dot): "FnTable:IVROverlay_027" (MOCK_OPENVR_OVERLAY=026: only _026; none: no
// IVROverlay at all). The slots the proxy uses record what they got; every other slot is a counting stub of its own (not
// g_stubCalls, which the fake game checks for the compositor). At shutdown: "mock overlay: ..." (VR_ShutdownInternal).
struct OvState {
    int64_t creates = 0, destroys = 0, shows = 0, hides = 0, textures = 0, alphas = 0, sorts = 0, widths = 0, transforms = 0, other = 0;
    std::string key, version;
    uint64_t handle = 0;
    float width = 0, x = 0, y = 0, z = 0, alphaMin = 2, alphaMax = -1;
    uint32_t device = 99;
    int texW = 0, texH = 0, r = -1, g = -1, b = -1, a = -1;   // the last texture: size + centre pixel (RGBA bytes)
    int64_t badHandle = 0;
};
OvState g_ov;
void* g_ovTable[64];
int OvCheck(uint64_t h) { if (!h || h != g_ov.handle) { ++g_ov.badHandle; return 10; } return 0; }   // 10 = InvalidHandle
int OvCreate(char* key, char* name, uint64_t* out) {
    (void)name;
    std::lock_guard<std::mutex> lk(g_m);
    ++g_ov.creates;
    g_ov.key = key ? key : "";
    g_ov.handle = 0x5EED0000ull + (uint64_t)g_ov.creates;
    if (out) *out = g_ov.handle;
    return 0;
}
int OvDestroy(uint64_t h) { std::lock_guard<std::mutex> lk(g_m); ++g_ov.destroys; const int e = OvCheck(h); g_ov.handle = 0; return e; }
int OvShow(uint64_t h) { std::lock_guard<std::mutex> lk(g_m); ++g_ov.shows; return OvCheck(h); }
int OvHide(uint64_t h) { std::lock_guard<std::mutex> lk(g_m); ++g_ov.hides; return OvCheck(h); }
int OvAlpha(uint64_t h, float a) {
    std::lock_guard<std::mutex> lk(g_m);
    ++g_ov.alphas;
    if (a < g_ov.alphaMin) g_ov.alphaMin = a;
    if (a > g_ov.alphaMax) g_ov.alphaMax = a;
    return OvCheck(h);
}
int OvSort(uint64_t h, uint32_t) { std::lock_guard<std::mutex> lk(g_m); ++g_ov.sorts; return OvCheck(h); }
int OvWidth(uint64_t h, float w) { std::lock_guard<std::mutex> lk(g_m); ++g_ov.widths; g_ov.width = w; return OvCheck(h); }
int OvTransform(uint64_t h, uint32_t dev, vr::HmdMatrix34_t* m) {
    std::lock_guard<std::mutex> lk(g_m);
    ++g_ov.transforms;
    g_ov.device = dev;
    if (m) { g_ov.x = m->m[0][3]; g_ov.y = m->m[1][3]; g_ov.z = m->m[2][3]; }
    return OvCheck(h);
}
// Reads the centre pixel back on the texture's own device (the proxy calls this inside the game's Submit, on its thread).
int OvTexture(uint64_t h, vr::Texture_t* t) {
    std::lock_guard<std::mutex> lk(g_m);
    ++g_ov.textures;
    if (!t || !t->handle || t->eType != vr::TextureType_DirectX) return 3;   // InvalidParameter
    ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(t->handle);
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    g_ov.texW = (int)d.Width;
    g_ov.texH = (int)d.Height;
    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopyResource(st, tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const uint8_t* p = (const uint8_t*)m.pData + (size_t)(d.Height / 2) * m.RowPitch + (size_t)(d.Width / 2) * 4;
            g_ov.r = p[0]; g_ov.g = p[1]; g_ov.b = p[2]; g_ov.a = p[3];
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    ctx->Release();
    dev->Release();
    return OvCheck(h);
}
template <int N> intptr_t OvStub() { std::lock_guard<std::mutex> lk(g_m); ++g_ov.other; return 0; }
template <int... I> void FillOvStubs(std::integer_sequence<int, I...>) { ((g_ovTable[I] = (void*)&OvStub<I>), ...); }
// slots as in src/openvr/openvr_versions.h kOverlayVersions (Valve's openvr_capi.h)
void* OverlayTable(const std::string& version) {
    FillOvStubs(std::make_integer_sequence<int, 64>{});
    const bool v26 = version == "IVROverlay_026";
    g_ovTable[1] = (void*)&OvCreate;
    g_ovTable[2] = (void*)&OvDestroy;
    g_ovTable[15] = (void*)&OvAlpha;
    g_ovTable[19] = (void*)&OvSort;
    g_ovTable[21] = (void*)&OvWidth;
    g_ovTable[34] = (void*)&OvTransform;
    g_ovTable[v26 ? 43 : 41] = (void*)&OvShow;
    g_ovTable[v26 ? 44 : 42] = (void*)&OvHide;
    g_ovTable[v26 ? 60 : 58] = (void*)&OvTexture;
    g_ov.version = version;
    return g_ovTable;
}

} // namespace

// ---------------------------------------------------------------- exports (Valve's names)

MOCK_EXPORT uint32_t VR_InitInternal2(vr::EVRInitError* e, vr::EVRApplicationType type, const char*) {
    if (type == vr::VRApplication_Background) {
        ++g_backgroundInits;
        if (Env("MOCK_OPENVR_NO_SERVER") == "1") {
            if (e) *e = vr::VRInitError_Init_NoServerForBackgroundApp;
            return 0;
        }
    }
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        g_initTick = c.QuadPart;
    }
    g_inited = true;
    if (e) *e = vr::VRInitError_None;
    return ++g_token;
}
MOCK_EXPORT uint32_t VR_InitInternal(vr::EVRInitError* e, vr::EVRApplicationType t) { return VR_InitInternal2(e, t, nullptr); }

MOCK_EXPORT void VR_ShutdownInternal() {
    ++g_shutdowns;
    g_inited = false;
    std::lock_guard<std::mutex> lk(g_m);
    printf("mock: submits L %lld R %lld, waitgetposes %lld, fntable submits %lld, errors %lld%s%s, stub calls %lld, accessor calls %lld, "
           "system calls %lld, mirror gets %lld releases %lld, events polled %lld, background inits %lld, interfaces asked: %s\n",
           (long long)g_submitL.load(), (long long)g_submitR.load(), (long long)g_wait.load(), (long long)g_fnSubmits.load(),
           (long long)g_invalid.load(), g_firstInvalid.empty() ? "" : " (first: ", g_firstInvalid.empty() ? "" : (g_firstInvalid + ")").c_str(),
           (long long)g_stubCalls.load(), (long long)g_accessorCalls.load(), (long long)g_systemCalls.load(),
           (long long)g_mirrorGets.load(), (long long)g_mirrorReleases.load(), (long long)g_events.load(),
           (long long)g_backgroundInits.load(), g_asked.c_str());
    printf("mock overlay: version %s, creates %lld key %s, width %.3f, transform dev %u at (%.2f, %.2f, %.2f), textures %lld "
           "(last %dx%d centre %d,%d,%d,%d), shows %lld hides %lld, alpha calls %lld (min %.2f max %.2f), sort %lld, destroys %lld, "
           "bad handles %lld, other slots %lld\n",
           g_ov.version.empty() ? "-" : g_ov.version.c_str(), (long long)g_ov.creates, g_ov.key.empty() ? "-" : g_ov.key.c_str(),
           g_ov.width, g_ov.device, g_ov.x, g_ov.y, g_ov.z, (long long)g_ov.textures, g_ov.texW, g_ov.texH, g_ov.r, g_ov.g, g_ov.b,
           g_ov.a, (long long)g_ov.shows, (long long)g_ov.hides, (long long)g_ov.alphas, g_ov.alphaMin, g_ov.alphaMax,
           (long long)g_ov.sorts, (long long)g_ov.destroys, (long long)g_ov.badHandle, (long long)g_ov.other);
    fflush(stdout);
}

MOCK_EXPORT void* VR_GetGenericInterface(const char* name, vr::EVRInitError* e) {
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (!g_asked.empty()) g_asked += " ";
        g_asked += name ? name : "(null)";
    }
    if (e) *e = vr::VRInitError_None;
    if (!g_inited) { if (e) *e = vr::VRInitError_Init_NotInitialized; return nullptr; }
    const std::string n = name ? name : "";
    const bool only019 = Env("MOCK_OPENVR_SYSTEM") == "019";
    if (n == "IVRCompositor_028" || n == "IVRCompositor_099") return &g_comp028;
    if (n == "IVRCompositor_022") return &g_comp022;
    if (n == "IVRCompositor_029") return &g_comp029;
    if (n == "FnTable:IVRCompositor_029") return g_fn029;
    if (n == "FnTable:IVRCompositor_028" || n == "FnTable:IVRCompositor_099") return g_fn028;
    if (n == "FnTable:IVRCompositor_022") return g_fn022;
    if (n == "IVRSystem_022" && !only019) return static_cast<vr::IVRSystem*>(&g_sys022);
    if (n == "IVRSystem_019") return &g_sys019;
    {
        const std::string ov = Env("MOCK_OPENVR_OVERLAY");   // "" = _027, "026" = only _026, "none" = no IVROverlay
        if (n == "FnTable:IVROverlay_027" && ov.empty()) return OverlayTable("IVROverlay_027");
        if (n == "FnTable:IVROverlay_026" && ov == "026") return OverlayTable("IVROverlay_026");
    }
    if (e) *e = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}

MOCK_EXPORT bool VR_IsInterfaceVersionValid(const char* name) {
    return name && (strncmp(name, "IVRCompositor_", 14) == 0 || strncmp(name, "IVRSystem_", 10) == 0);
}
MOCK_EXPORT uint32_t VR_GetInitToken() { return g_token; }
MOCK_EXPORT bool VR_IsHmdPresent() { return true; }
MOCK_EXPORT bool VR_IsRuntimeInstalled() { return true; }
MOCK_EXPORT const char* VR_RuntimePath() { return "C:\\mock\\SteamVR"; }
MOCK_EXPORT bool VR_GetRuntimePath(char* buf, uint32_t size, uint32_t* need) {
    const char* p = "C:\\mock\\SteamVR";
    const uint32_t n = (uint32_t)strlen(p) + 1;
    if (need) *need = n;
    if (!buf || size < n) return false;
    memcpy(buf, p, n);
    return true;
}
MOCK_EXPORT const char* VR_GetVRInitErrorAsSymbol(vr::EVRInitError e) { return e == vr::VRInitError_None ? "VRInitError_None" : "VRInitError_Mock"; }
MOCK_EXPORT const char* VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError e) { return e == vr::VRInitError_None ? "No Error (mock)" : "Mock error"; }
MOCK_EXPORT const char* VR_GetStringForHmdError(vr::EVRInitError e) { return VR_GetVRInitErrorAsEnglishDescription(e); }

// Interface accessors (real signature unknown to the proxy): the test calls them through the proxy's jump stubs.
// LiquidVR: four doubles (xmm0..xmm3) -> a*1000 + b*100 + c*10 + d. VRPaths: four integers + a fifth on the stack ->
// a combination of all five. The rest: () -> a fixed pointer.
MOCK_EXPORT double LiquidVR(double a, double b, double c, double d) { ++g_accessorCalls; return a * 1000 + b * 100 + c * 10 + d; }
MOCK_EXPORT uint64_t VRPaths(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    ++g_accessorCalls;
    return a * 10000 + b * 1000 + c * 100 + d * 10 + e;
}
#define ACC(name) MOCK_EXPORT void* name() { ++g_accessorCalls; return &g_dummyAccessor[0]; }
ACC(VRCompositorSystemInternal)
ACC(VRControlPanel)
ACC(VRDriverDirectInternal)
ACC(VRHeadsetView)
ACC(VRRenderModelsInternal)
ACC(VRSceneGraph)
ACC(VRTrackedCameraInternal)
ACC(VRVirtualDisplay)
ACC(VRDashboardManager)   // older Valve DLLs only (the proxy passes them through like the rest)
ACC(VROculusDirect)
ACC(VROverlayView)
#undef ACC

// Test only: out[0..8] = submits L, R, waitgetposes, fntable submits, invalid, stub calls, accessor calls, shutdowns,
// system calls.
MOCK_EXPORT void MockOpenVrCounts(int64_t* out) {
    out[0] = g_submitL; out[1] = g_submitR; out[2] = g_wait; out[3] = g_fnSubmits; out[4] = g_invalid;
    out[5] = g_stubCalls; out[6] = g_accessorCalls; out[7] = g_shutdowns; out[8] = g_systemCalls;
}
MOCK_EXPORT void* MockOpenVrDummyAccessor() { return &g_dummyAccessor[0]; }
