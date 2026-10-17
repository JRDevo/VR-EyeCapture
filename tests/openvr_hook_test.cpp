// openvr_hook_test -- CPU unit test of the SteamVR proxy's helpers (src/openvr/openvr_hooks.cpp, openvr_versions.h):
// interface name parsing, the slot table, vtable patch / restore on a read-only table and on a real C++ object, the
// FnTable copy at a page edge, raw IVRSystem calls against the compiler's virtual-call ABI, HmdMatrix34 -> pose, compose,
// GetProjectionRaw -> fov, bounds -> rectangle, the colour-space / typed-format tables, the sidecar and the home
// discovery (sidecar, then HKCU\Software\OpenXR-EyeCapture-Test\... through XR_EYE_CAPTURE_TEST_HOME_KEY -- never the real key).
// Exit 0 = all passed.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "openvr/openvr_hooks.h"
#include "openvr/openvr_versions.h"

namespace {

int g_fail = 0, g_pass = 0;
void Check(bool ok, const char* what) {
    if (ok) ++g_pass; else { ++g_fail; printf("  FAIL %s\n", what); }
}
bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

// ---- a read-only vtable + an object on it
int g_origCalls = 0, g_hookCalls = 0;
int OrigFn(void*, int x) { ++g_origCalls; return x + 1; }
int HookFn(void*, int x) { ++g_hookCalls; return x + 100; }
int OtherFn(void*, int x) { return x + 1000; }

// ---- a real C++ class (compiler-made vtable in .rdata)
class Thing {
public:
    virtual int A(int x) { return x * 2; }
    virtual int B(int x) { return x * 3; }
};
int ThingHook(void*, int x) { return x * 7; }

// ---- IVRSystem implemented as a C++ class: the raw slot calls must see the same results
class Sys : public vr::IVRSystem {
public:
    void GetRecommendedRenderTargetSize(uint32_t*, uint32_t*) override {}
    vr::HmdMatrix44_t GetProjectionMatrix(vr::EVREye, float, float) override { return vr::HmdMatrix44_t{}; }
    void GetProjectionRaw(vr::EVREye eye, float* l, float* r, float* t, float* b) override {
        *l = eye ? -0.9f : -1.1f; *r = eye ? 1.1f : 0.9f; *t = -1.0f; *b = 0.95f;
    }
    bool ComputeDistortion(vr::EVREye, float, float, vr::DistortionCoordinates_t*) override { return false; }
    vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eye) override {
        vr::HmdMatrix34_t m{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) m.m[i][j] = (float)(i * 4 + j) + (eye ? 0.5f : 0.25f);
        return m;
    }
    bool GetTimeSinceLastVsync(float*, uint64_t*) override { return false; }
    int32_t GetD3D9AdapterIndex() override { return 0; }
    void GetDXGIOutputInfo(int32_t*) override {}
    void GetOutputDevice(uint64_t*, vr::ETextureType, vr::VkInstance_T*) override {}
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
    int GetTrackedDeviceClass(vr::TrackedDeviceIndex_t) override { return 0; }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t) override { return true; }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) override {
        *e = vr::TrackedProp_WrongDataType; return false;
    }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, vr::ETrackedPropertyError* e) override {
        if (d == 0 && p == vr::Prop_DisplayFrequency_Float) { *e = vr::TrackedProp_Success; return 72.0f; }
        *e = vr::TrackedProp_UnknownProperty; return 0;
    }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError*) override { return 0; }
    uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError*) override { return 0; }
    vr::HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError*) override {
        return vr::HmdMatrix34_t{};
    }
    uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, uint32_t, void*, uint32_t,
                                           vr::ETrackedPropertyError*) override { return 0; }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, char*, uint32_t,
                                            vr::ETrackedPropertyError*) override { return 0; }
    const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return ""; }
    bool PollNextEvent(vr::VREvent_t*, uint32_t) override { return false; }
};

vr::HmdMatrix34_t RotY(double deg, float tx, float ty, float tz) {
    const double r = deg * 3.14159265358979 / 180.0;
    vr::HmdMatrix34_t m{};
    m.m[0][0] = (float)std::cos(r); m.m[0][2] = (float)std::sin(r);
    m.m[1][1] = 1;
    m.m[2][0] = (float)-std::sin(r); m.m[2][2] = (float)std::cos(r);
    m.m[0][3] = tx; m.m[1][3] = ty; m.m[2][3] = tz;
    return m;
}

std::wstring TempDir(const wchar_t* name) {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::wstring d = std::wstring(t) + name;
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void WriteFileBytes(const std::wstring& p, const std::string& bytes) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w = 0;
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr); CloseHandle(h); }
}

} // namespace

int main() {
    printf("openvr_hook_test\n");

    // ---- versions
    {
        bool fn = true, comp = false;
        int v = FindCompositorVersion("IVRCompositor_028", &fn, &comp);
        Check(v >= 0 && strcmp(kCompositorVersions[v].name, "IVRCompositor_028") == 0 && !fn && comp, "IVRCompositor_028 parsed (C++)");
        v = FindCompositorVersion("FnTable:IVRCompositor_022", &fn, &comp);
        Check(v >= 0 && strcmp(kCompositorVersions[v].name, "IVRCompositor_022") == 0 && fn && comp, "FnTable:IVRCompositor_022 parsed");
        v = FindCompositorVersion("IVRCompositor_099", &fn, &comp);
        Check(v == -1 && comp, "unknown IVRCompositor_099 -> -1, still a compositor");
        v = FindCompositorVersion("IVRSystem_022", &fn, &comp);
        Check(v == -1 && !comp, "IVRSystem_022 is not a compositor");
        v = FindCompositorVersion("FnTable:IVRSystem_022", &fn, &comp);
        Check(v == -1 && !comp && fn, "FnTable:IVRSystem_022 is not a compositor");
        Check(FindCompositorVersion(nullptr, &fn, &comp) == -1, "null name");
        bool slotsOk = true, countsOk = true;
        for (int i = 0; i < kCompositorVersionCount; ++i) {
            slotsOk = slotsOk && kCompositorVersions[i].slotWaitGetPoses == 2 && kCompositorVersions[i].slotSubmit == 5;
            countsOk = countsOk && kCompositorVersions[i].methodCount > 6 &&
                       kCompositorVersions[i].methodCount + kFnTableCopyMargin <= kFnTableMax &&
                       (i == 0 || kCompositorVersions[i].methodCount >= kCompositorVersions[i - 1].methodCount);
        }
        Check(slotsOk, "every known IVRCompositor: WaitGetPoses slot 2, Submit slot 5");
        Check(countsOk, "method counts plausible, non-decreasing, copy fits kFnTableMax");
        Check(kCompositorVersionCount == 10 && strcmp(kCompositorVersions[0].name, "IVRCompositor_019") == 0 &&
              strcmp(kCompositorVersions[9].name, "IVRCompositor_028") == 0, "versions 019 .. 028");
        Check(kSystemVersions[0].slotFloatProperty == 22 && kSystemVersions[3].slotFloatProperty == 23 &&
              kSystemVersions[0].slotProjectionRaw == 2 && kSystemVersions[0].slotEyeToHead == 4, "IVRSystem slot table");
    }

    // ---- vtable patch / restore on a read-only page
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        void** vt = (void**)VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        for (int i = 0; i < 8; ++i) vt[i] = (void*)&OtherFn;
        vt[5] = (void*)&OrigFn;
        DWORD old = 0;
        VirtualProtect(vt, si.dwPageSize, PAGE_READONLY, &old);
        struct Obj { void** vtbl; } obj{vt};
        typedef int (*Fn)(void*, int);
        auto call = [&](int x) { return ((Fn)obj.vtbl[5])(&obj, x); };
        Check(call(1) == 2 && g_origCalls == 1, "original slot before the patch");
        void* orig = nullptr;
        Check(PatchVtableSlot(vt, 5, (void*)&HookFn, &orig) && orig == (void*)&OrigFn, "patch a read-only vtable slot");
        Check(call(1) == 101 && g_hookCalls == 1, "patched slot calls the hook");
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(vt, &mbi, sizeof(mbi));
        Check(mbi.Protect == PAGE_READONLY, "page protection restored after the patch");
        void* cur = nullptr;
        Check(RestoreVtableSlot(vt, 5, (void*)&HookFn, orig, &cur) && cur == (void*)&HookFn, "restore");
        Check(call(1) == 2, "restored slot calls the original");
        // someone else patched over us: restore must leave it alone
        PatchVtableSlot(vt, 5, (void*)&OtherFn, nullptr);
        Check(!RestoreVtableSlot(vt, 5, (void*)&HookFn, orig, &cur) && cur == (void*)&OtherFn && vt[5] == (void*)&OtherFn,
              "restore refuses when the slot holds someone else's pointer");
        Check(!PatchVtableSlot(nullptr, 5, (void*)&HookFn, &orig), "null vtable refused");
        VirtualFree(vt, 0, MEM_RELEASE);
    }

    // ---- VtablePatchTable: a call that entered a hook before the restore still forwards; re-init re-arms the same record
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        void** vt = (void**)VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        for (int i = 0; i < 8; ++i) vt[i] = (void*)&OtherFn;
        vt[2] = (void*)&OrigFn;
        vt[5] = (void*)&OrigFn;
        DWORD old = 0;
        VirtualProtect(vt, si.dwPageSize, PAGE_READONLY, &old);
        struct Obj { void** vtbl; } obj{vt};
        typedef int (*Fn)(void*, int);
        static VtablePatchTable* s_tbl = nullptr;
        static VtablePatchTable tbl;
        s_tbl = &tbl;
        // the proxy's hook pattern: find the record by the object's vtable, forward to the saved original (+100 while armed)
        struct H {
            static int Hook(void* self, int x) {
                const VtablePatch* r = s_tbl->Find(*(void***)self);
                if (!r || !r->origSubmit) return -1;
                const int v = ((Fn)r->origSubmit)(self, x);
                return r->restored ? v : v + 100;
            }
        };
        const VtablePatch* rec = nullptr;
        Check(tbl.ArmVtable(vt, 9, 2, 5, (void*)&H::Hook, (void*)&H::Hook, &rec) == VtablePatchTable::Arm::Armed && rec && tbl.Count() == 1 &&
                  ((Fn)vt[5])(&obj, 1) == 102, "armed: the virtual call goes through the hook");
        Check(tbl.ArmVtable(vt, 9, 2, 5, (void*)&H::Hook, (void*)&H::Hook) == VtablePatchTable::Arm::AlreadyArmed && tbl.Count() == 1,
              "the same vtable again while armed: already armed");
        int kept = -1;
        Check(tbl.RestoreAll((void*)&H::Hook, (void*)&H::Hook, &kept) == 1 && kept == 0 && vt[5] == (void*)&OrigFn && vt[2] == (void*)&OrigFn,
              "shutdown: both slots restored");
        Check(tbl.Find(vt) == rec && rec->restored && rec->vtable == vt, "the record keeps its vtable after the restore");
        Check(H::Hook(&obj, 1) == 2, "a call already inside the hook after the restore still reaches the original (not -1)");
        for (int cycle = 0; cycle < 40; ++cycle) {   // many VR init / shutdown cycles with the same vrclient
            const VtablePatchTable::Arm a = tbl.ArmVtable(vt, 9, 2, 5, (void*)&H::Hook, (void*)&H::Hook);
            if (a != VtablePatchTable::Arm::Rearmed || ((Fn)vt[5])(&obj, 1) != 102 || tbl.RestoreAll((void*)&H::Hook, (void*)&H::Hook) != 1) {
                printf("    re-init cycle %d failed\n", cycle);
                Check(false, "re-init cycle");
                break;
            }
        }
        Check(tbl.Count() == 1, "40 re-init cycles: still one record (no exhaustion)");
        // other vtables: a full table reuses a restored record
        void** other[VtablePatchTable::kMax + 1];
        for (int i = 0; i <= VtablePatchTable::kMax; ++i) {
            other[i] = (void**)VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            for (int k = 0; k < 8; ++k) other[i][k] = (void*)&OrigFn;
        }
        for (int i = 0; i < VtablePatchTable::kMax - 1; ++i) tbl.ArmVtable(other[i], 9, 2, 5, (void*)&H::Hook, (void*)&H::Hook);
        Check(tbl.Count() == VtablePatchTable::kMax, "table filled (16 records)");
        Check(tbl.ArmVtable(other[VtablePatchTable::kMax - 1], 9, 2, 5, (void*)&H::Hook, (void*)&H::Hook) == VtablePatchTable::Arm::Armed &&
                  tbl.Find(vt) == nullptr && tbl.Count() == VtablePatchTable::kMax,
              "full: a new vtable reuses the restored record of the old one");
        Check(tbl.ArmVtable(other[VtablePatchTable::kMax], 9, 2, 5, (void*)&H::Hook, (void*)&H::Hook) == VtablePatchTable::Arm::Full,
              "full with every record armed: refused (Full)");
        for (int i = 0; i <= VtablePatchTable::kMax; ++i) VirtualFree(other[i], 0, MEM_RELEASE);
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(vt, &mbi, sizeof(mbi));
        Check(mbi.Protect == PAGE_READONLY, "the data page stays PAGE_READONLY after the patches (patched as PAGE_READWRITE)");
        VirtualFree(vt, 0, MEM_RELEASE);
    }

    // ---- patch the compiler's vtable of a real C++ object, call it as a normal virtual call
    {
        Thing* t = new Thing();
        void** vt = *(void***)t;
        void* orig = nullptr;
        Check(t->B(2) == 6, "C++ object before the patch");
        Check(PatchVtableSlot(vt, 1, (void*)&ThingHook, &orig), "patch a C++ vtable (.rdata)");
        Check(t->B(2) == 14 && t->A(2) == 4, "virtual call goes through the hook, the other slot untouched");
        Check(RestoreVtableSlot(vt, 1, (void*)&ThingHook, orig, nullptr) && t->B(2) == 6, "C++ vtable restored");
        delete t;
    }

    // ---- FnTable copy at a page edge (the next page is not readable)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        char* base = (char*)VirtualAlloc(nullptr, 2 * si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old = 0;
        VirtualProtect(base + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &old);
        void** tbl = (void**)(base + si.dwPageSize) - 10;   // 10 entries before the guard page
        for (int i = 0; i < 10; ++i) tbl[i] = (void*)(uintptr_t)(0x1000 + i);
        void* dst[kFnTableMax] = {};
        const uint32_t n = CopyFnTable(tbl, 40, dst);
        Check(n == 10 && dst[9] == (void*)(uintptr_t)0x1009, "FnTable copy stops at an unreadable page");
        Check(CopyFnTable(tbl, 5, dst) == 5, "FnTable copy of a short count");
        Check(CopyFnTable(nullptr, 5, dst) == 0, "FnTable copy of null");
        VirtualFree(base, 0, MEM_RELEASE);
    }

    // ---- raw IVRSystem calls == virtual calls
    {
        Sys sys;
        vr::IVRSystem* is = &sys;
        const SystemVersion& v = kSystemVersions[0];   // IVRSystem_022
        float l = 0, r = 0, t = 0, b = 0;
        SysGetProjectionRaw(is, v, vr::Eye_Right, &l, &r, &t, &b);
        Check(l == -0.9f && r == 1.1f && t == -1.0f && b == 0.95f, "SysGetProjectionRaw by slot 2");
        const vr::HmdMatrix34_t m1 = SysGetEyeToHead(is, v, vr::Eye_Left);
        const vr::HmdMatrix34_t m2 = is->GetEyeToHeadTransform(vr::Eye_Left);
        Check(memcmp(&m1, &m2, sizeof(m1)) == 0 && m1.m[2][3] == 11.25f, "SysGetEyeToHead (struct return by slot 4) == virtual call");
        float hz = 0;
        int err = -1;
        Check(SysGetDisplayHz(is, v, &hz, &err) && hz == 72.0f && err == 0, "SysGetDisplayHz by slot 22");
        SystemVersion wrong = v;
        wrong.slotFloatProperty = 21;   // the bool getter (an off-by-one slot): rejected by the error code
        Check(!SysGetDisplayHz(is, wrong, &hz, &err) && err == vr::TrackedProp_WrongDataType, "wrong slot rejected by its error code");
    }

    // ---- matrices
    {
        float q[4], p[3];
        vr::HmdMatrix34_t id = RotY(0, 0, 1.6f, 0);
        Check(MatrixToPose(id, q, p) && Near(q[3], 1) && Near(q[0], 0) && Near(p[1], 1.6), "identity -> (0,0,0,1) at (0,1.6,0)");
        Check(MatrixToPose(RotY(90, 0, 0, 0), q, p) && Near(q[1], std::sqrt(0.5)) && Near(q[3], std::sqrt(0.5)) && Near(q[0], 0),
              "90 deg about +Y -> (0, .707, 0, .707)");
        Check(MatrixToPose(RotY(180, 0, 0, 0), q, p) && Near(std::fabs(q[1]), 1) && Near(q[3], 0, 1e-4), "180 deg about +Y");
        vr::HmdMatrix34_t bad{};
        Check(!MatrixToPose(bad, q, p), "zero matrix rejected");
        bad = id;
        bad.m[0][3] = NAN;
        Check(!MatrixToPose(bad, q, p), "NaN position rejected");
        // eye = head * eyeToHead: head turned 90 deg left (+Y), eye 3.2 cm to the right -> the eye ends up at -Z of the head
        const vr::HmdMatrix34_t head = RotY(90, 1, 2, 3);
        vr::HmdMatrix34_t e2h = RotY(0, 0.032f, 0, 0);
        const vr::HmdMatrix34_t eye = ComposeMatrix(head, e2h);
        Check(Near(eye.m[0][3], 1) && Near(eye.m[1][3], 2) && Near(eye.m[2][3], 3 - 0.032), "compose: eye offset rotated with the head");
        Check(Near(eye.m[0][2], 1) && Near(eye.m[2][0], -1), "compose: rotation kept");
    }

    // ---- fov
    {
        float fov[4];
        FovFromProjectionRaw(-1.1f, 0.9f, -1.0f, 0.9f, fov);
        Check(Near(fov[0], std::atan(-1.1)) && Near(fov[1], std::atan(0.9)) && Near(fov[2], std::atan(1.0)) && Near(fov[3], std::atan(-0.9)),
              "GetProjectionRaw -> {atan l, atan r, atan -t, atan -b}");
        Check(fov[0] < 0 && fov[1] > 0 && fov[2] > 0 && fov[3] < 0, "fov signs: left/down negative, right/up positive (OpenXR)");
    }

    // ---- bounds
    {
        int32_t x = -1, y = -1;
        uint32_t w = 0, h = 0;
        bool fu = true, fv = true;
        Check(BoundsToRect(nullptr, 1920, 1080, &x, &y, &w, &h, &fu, &fv) && x == 0 && y == 0 && w == 1920 && h == 1080 && !fu && !fv,
              "null bounds = whole texture");
        vr::VRTextureBounds_t b{0.5f, 0, 1, 1};
        Check(BoundsToRect(&b, 3840, 1080, &x, &y, &w, &h, &fu, &fv) && x == 1920 && w == 1920 && h == 1080, "right half of a wide texture");
        b = {0, 1, 0.5f, 0};
        Check(BoundsToRect(&b, 3840, 1080, &x, &y, &w, &h, &fu, &fv) && fv && !fu && x == 0 && y == 0 && w == 1920 && h == 1080,
              "vMin > vMax: flipped, un-flipped rectangle");
        b = {-0.2f, 0.1f, 1.3f, 0.9f};
        Check(BoundsToRect(&b, 1000, 1000, &x, &y, &w, &h, &fu, &fv) && x == 0 && w == 1000 && y == 100 && h == 800, "clipped to 0..1");
        b = {0.3f, 0, 0.3f, 1};
        Check(!BoundsToRect(&b, 1000, 1000, &x, &y, &w, &h, &fu, &fv), "empty bounds refused");
        b = {NAN, 0, 1, 1};
        Check(!BoundsToRect(&b, 1000, 1000, &x, &y, &w, &h, &fu, &fv), "NaN bounds refused");
    }

    // ---- colour space / formats
    {
        Check(SubmitIsSrgb(vr::ColorSpace_Gamma, DXGI_FORMAT_R16G16B16A16_FLOAT), "Gamma -> sRGB whatever the format");
        Check(!SubmitIsSrgb(vr::ColorSpace_Linear, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), "Linear -> linear whatever the format");
        Check(SubmitIsSrgb(vr::ColorSpace_Auto, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), "Auto + *_SRGB -> sRGB");
        Check(SubmitIsSrgb(vr::ColorSpace_Auto, DXGI_FORMAT_R8G8B8A8_TYPELESS), "Auto + 8-bit TYPELESS -> sRGB (gamma)");
        Check(SubmitIsSrgb(vr::ColorSpace_Auto, DXGI_FORMAT_R8G8B8A8_UNORM), "Auto + 8-bit UNORM -> sRGB (gamma)");
        Check(!SubmitIsSrgb(vr::ColorSpace_Auto, DXGI_FORMAT_R16G16B16A16_FLOAT), "Auto + float -> linear");
        Check(!SubmitIsSrgb(vr::ColorSpace_Auto, DXGI_FORMAT_R10G10B10A2_UNORM), "Auto + 10-bit -> linear");
        Check(TypedCaptureFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS, true) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
              TypedCaptureFormat(DXGI_FORMAT_B8G8R8A8_TYPELESS, false) == DXGI_FORMAT_B8G8R8A8_UNORM &&
              TypedCaptureFormat(DXGI_FORMAT_R16G16B16A16_TYPELESS, false) == DXGI_FORMAT_R16G16B16A16_FLOAT &&
              TypedCaptureFormat(DXGI_FORMAT_R10G10B10A2_TYPELESS, true) == DXGI_FORMAT_R10G10B10A2_UNORM &&
              TypedCaptureFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false) == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
              "TYPELESS -> typed capture format (typed formats unchanged)");
    }

    // ---- sidecar + home discovery (test registry key only)
    {
        const std::wstring proxyDir = TempDir(L"eyecap_hooktest_proxy");
        const std::wstring home = TempDir(L"eyecap_hooktest_home \u00dc");   // a space and a non-ASCII character
        const std::wstring sidecar = proxyDir + L"\\" EYECAP_PROXY_SIDECAR;
        const std::wstring testKey = L"Software\\OpenXR-EyeCapture-Test\\ProxyHome" + std::to_wstring(GetCurrentProcessId());
        SetEnvironmentVariableW(EYECAP_TEST_HOME_KEY_ENV, testKey.c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
        DeleteFileW(sidecar.c_str());
        ProxyHome ph;
        Check(!DiscoverProxyHome(proxyDir, ph) && ph.home.empty(), "no sidecar, no registry value -> no home");
        WriteFileBytes(sidecar, ProxySidecarText(home + L"\\", L"Crisis VRigade 2"));
        ProxySidecar sc;
        Check(ReadProxySidecar(sidecar, sc) && sc.home == home && sc.game == L"Crisis VRigade 2", "sidecar round trip (UTF-8, trailing slash dropped)");
        Check(DiscoverProxyHome(proxyDir, ph) && ph.home == home && ph.game == L"Crisis VRigade 2" && ph.from.find(L"sidecar") == 0,
              "home from the sidecar");
        WriteFileBytes(sidecar, "\xEF\xBB\xBF; comment\r\n[x]\r\nHOME = \"" + std::string("C:\\no\\such\\folder") + "\"\r\n");
        Check(ReadProxySidecar(sidecar, sc) && sc.home == L"C:\\no\\such\\folder", "sidecar with BOM, comment, quotes, upper-case key");
        Check(!DiscoverProxyHome(proxyDir, ph), "sidecar home that does not exist and no registry value -> no home");
        HKEY k = nullptr;
        RegCreateKeyExW(HKEY_CURRENT_USER, testKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
        if (k) {
            RegSetValueExW(k, EYECAP_HOME_REG_VALUE, 0, REG_SZ, (const BYTE*)home.c_str(), (DWORD)((home.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
        Check(DiscoverProxyHome(proxyDir, ph) && ph.home == home && ph.from.find(L"registry HKCU\\Software\\OpenXR-EyeCapture-Test") == 0,
              "home from the (test) registry value when the sidecar's home is missing");
        DeleteFileW(sidecar.c_str());
        Check(DiscoverProxyHome(proxyDir, ph) && ph.home == home, "home from the registry without a sidecar");
        RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test");   // only if empty (other tests may use it)
        Check(!DiscoverProxyHome(proxyDir, ph), "test key removed -> no home");
        RemoveDirectoryW(proxyDir.c_str());
        RemoveDirectoryW(home.c_str());
    }

    printf("openvr_hook_test: %d passed, %d failed -> %s\n", g_pass, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
