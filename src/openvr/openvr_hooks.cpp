// openvr_hooks -- see openvr_hooks.h. No logging here (the tray and the unit test link this file too).
#include "openvr_hooks.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring StripSlash(std::wstring s) {
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

} // namespace

// ------------------------------------------------------------------------------------------------ home discovery

bool ReadProxySidecar(const std::wstring& path, ProxySidecar& out) {
    out = ProxySidecar();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    std::wstringstream ws(Utf8ToWide(bytes));
    std::wstring line;
    while (std::getline(ws, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#' || line[0] == L'[') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        for (auto& c : key) c = (wchar_t)towlower(c);
        std::wstring val = Trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == L'"' && val.back() == L'"') val = val.substr(1, val.size() - 2);
        if (key == L"home") out.home = StripSlash(val);
        else if (key == L"game") out.game = val;
    }
    return !out.home.empty();
}

std::string ProxySidecarText(const std::wstring& home, const std::wstring& game) {
    std::string s;
    s += "; VR EyeCapture SteamVR proxy: where the recorder's config.ini, logs and captures are (written by the tray's\r\n";
    s += "; \"SteamVR games\" Enable; removed by Disable together with the proxy openvr_api.dll).\r\n";
    s += "home = " + WideToUtf8(home) + "\r\n";
    s += "game = " + WideToUtf8(game) + "\r\n";
    return s;
}

bool DiscoverProxyHome(const std::wstring& proxyDir, ProxyHome& out) {
    out = ProxyHome();
    const std::wstring sidecar = proxyDir + L"\\" EYECAP_PROXY_SIDECAR;
    ProxySidecar sc;
    if (ReadProxySidecar(sidecar, sc) && IsDir(sc.home)) {
        out.home = sc.home;
        out.game = sc.game;
        out.from = L"sidecar " + sidecar;
        return true;
    }
    wchar_t keyBuf[512];
    const DWORD n = GetEnvironmentVariableW(EYECAP_TEST_HOME_KEY_ENV, keyBuf, 512);
    const std::wstring key = (n > 0 && n < 512) ? std::wstring(keyBuf) : std::wstring(EYECAP_HOME_REG_KEY);
    wchar_t val[1024] = L"";
    DWORD bytes = sizeof(val) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), EYECAP_HOME_REG_VALUE, RRF_RT_REG_SZ, &type, val, &bytes) == ERROR_SUCCESS) {
        const std::wstring home = StripSlash(Trim(val));
        if (!home.empty() && IsDir(home)) {
            out.home = home;
            out.from = L"registry HKCU\\" + key + L"\\" EYECAP_HOME_REG_VALUE;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ patching

namespace {
// Writable protection for the page of `at`: keeps execute only when the page already had it (a vtable lives in .rdata).
DWORD WritableProtection(void* at) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(at, &mbi, sizeof(mbi))) return PAGE_READWRITE;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
}
} // namespace

bool PatchVtableSlot(void** vtable, uint32_t slot, void* hook, void** original) {
    if (!vtable) return false;
    void** at = vtable + slot;
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(void*), WritableProtection(at), &old)) return false;
    void* prev = InterlockedExchangePointer(at, hook);
    DWORD tmp = 0;
    VirtualProtect(at, sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(void*));
    if (original) *original = prev;
    return true;
}

bool RestoreVtableSlot(void** vtable, uint32_t slot, void* hook, void* original, void** current) {
    if (!vtable) return false;
    void** at = vtable + slot;
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(void*), WritableProtection(at), &old)) {
        if (current) *current = nullptr;
        return false;
    }
    void* prev = InterlockedCompareExchangePointer(at, original, hook);
    DWORD tmp = 0;
    VirtualProtect(at, sizeof(void*), old, &tmp);
    if (current) *current = prev;
    return prev == hook;
}

const VtablePatch* VtablePatchTable::Find(void** vtable) const {
    const long n = n_;
    for (long i = 0; i < n; ++i)
        if (rec_[i].vtable == vtable) return &rec_[i];
    return nullptr;
}

VtablePatchTable::Arm VtablePatchTable::ArmVtable(void** vt, int version, uint32_t slotWait, uint32_t slotSubmit, void* hookWait,
                                                  void* hookSubmit, const VtablePatch** out) {
    if (out) *out = nullptr;
    VtablePatch* r = const_cast<VtablePatch*>(Find(vt));
    Arm result = Arm::Armed;
    if (r && !r->restored) {
        if (out) *out = r;
        return Arm::AlreadyArmed;
    }
    if (r) {
        result = Arm::Rearmed;   // same vtable again (a re-init of the same vrclient): the record is reused
    } else if (n_ < kMax) {
        r = &rec_[n_];
        *r = VtablePatch();
        r->restored = true;      // not armed yet: an early call through Find would only forward
        r->vtable = vt;
        MemoryBarrier();
        InterlockedIncrement(&n_);
    } else {
        for (int i = 0; i < kMax && !r; ++i)
            if (rec_[i].restored) r = &rec_[i];   // table full: a restored record of an unloaded vtable
        if (!r) return Arm::Full;
        r->vtable = vt;
    }
    r->version = version;
    r->slotWait = slotWait;
    r->slotSubmit = slotSubmit;
    r->origWait = vt[slotWait];      // read before the hooks go in (an in-flight hook call forwards to these)
    r->origSubmit = vt[slotSubmit];
    MemoryBarrier();
    void* o1 = nullptr;
    void* o2 = nullptr;
    const bool ok1 = PatchVtableSlot(vt, slotWait, hookWait, &o1);
    const bool ok2 = ok1 && PatchVtableSlot(vt, slotSubmit, hookSubmit, &o2);
    if (!ok1 || !ok2) {
        if (ok1) RestoreVtableSlot(vt, slotWait, hookWait, r->origWait, nullptr);
        r->restored = true;
        return Arm::ProtectFailed;
    }
    r->restored = false;
    if (out) *out = r;
    return result;
}

int VtablePatchTable::RestoreAll(void* hookWait, void* hookSubmit, int* kept) {
    int restored = 0, k = 0;
    const long n = n_;
    for (long i = 0; i < n; ++i) {
        VtablePatch& r = rec_[i];
        if (r.restored) continue;
        void* cur = nullptr;
        const bool a = RestoreVtableSlot(r.vtable, r.slotWait, hookWait, r.origWait, &cur);
        const bool b = RestoreVtableSlot(r.vtable, r.slotSubmit, hookSubmit, r.origSubmit, &cur);
        if (a && b) { r.restored = true; ++restored; }   // the vtable stays: an in-flight call still finds the originals
        else ++k;
    }
    if (kept) *kept = k;
    return restored;
}

namespace {
bool ReadEntry(void* const* src, uint32_t i, void** out) {
    __try {
        *out = src[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

uint32_t CopyFnTable(void* const* src, uint32_t n, void** dst) {
    if (!src || !dst) return 0;
    uint32_t i = 0;
    for (; i < n; ++i) {
        void* e = nullptr;
        if (!ReadEntry(src, i, &e)) break;
        dst[i] = e;
    }
    return i;
}

// ------------------------------------------------------------------------------------------------ IVRSystem (raw slots)
// Called through the vtable slot as a plain function: `this` first; a method that returns a struct by value gets the
// hidden result pointer right after `this` (MSVC x64 member-function convention; openvr_hook_test checks it against a
// real C++ virtual call).

void SysGetProjectionRaw(void* sys, const SystemVersion& v, vr::EVREye eye, float* l, float* r, float* t, float* b) {
    typedef void (*Fn)(void*, vr::EVREye, float*, float*, float*, float*);
    Fn fn = (Fn)(*(void***)sys)[v.slotProjectionRaw];
    fn(sys, eye, l, r, t, b);
}

vr::HmdMatrix34_t SysGetEyeToHead(void* sys, const SystemVersion& v, vr::EVREye eye) {
    typedef vr::HmdMatrix34_t* (*Fn)(void*, vr::HmdMatrix34_t*, vr::EVREye);
    Fn fn = (Fn)(*(void***)sys)[v.slotEyeToHead];
    vr::HmdMatrix34_t m{};
    fn(sys, &m, eye);
    return m;
}

bool SysGetDisplayHz(void* sys, const SystemVersion& v, float* hz, int* error) {
    typedef float (*Fn)(void*, vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError*);
    Fn fn = (Fn)(*(void***)sys)[v.slotFloatProperty];
    vr::ETrackedPropertyError err = vr::TrackedProp_UnknownProperty;
    const float f = fn(sys, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &err);
    if (error) *error = (int)err;
    if (hz) *hz = f;
    return err == vr::TrackedProp_Success && std::isfinite(f) && f > 20.0f && f < 1000.0f;
}

// ------------------------------------------------------------------------------------------------ conversions

double MatrixRotationError(const vr::HmdMatrix34_t& M) {
    double err = 0;
    for (int i = 0; i < 3; ++i) {
        for (int j = i; j < 3; ++j) {
            const double d = M.m[0][i] * M.m[0][j] + M.m[1][i] * M.m[1][j] + M.m[2][i] * M.m[2][j];
            err = std::max(err, std::fabs(d - (i == j ? 1.0 : 0.0)));
        }
    }
    const double det = M.m[0][0] * (M.m[1][1] * M.m[2][2] - M.m[1][2] * M.m[2][1]) -
                       M.m[0][1] * (M.m[1][0] * M.m[2][2] - M.m[1][2] * M.m[2][0]) +
                       M.m[0][2] * (M.m[1][0] * M.m[2][1] - M.m[1][1] * M.m[2][0]);
    if (!(det > 0)) return std::isfinite(det) ? std::max(err, 1.0) : INFINITY;   // a mirror or a flat matrix: never a rotation
    return std::isfinite(err) ? err : INFINITY;
}

double MatrixAngleDeg(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b) {
    double tr = 0;   // trace(Ra^T Rb) = 1 + 2 cos(angle)
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) tr += a.m[k][i] * b.m[k][i];
    const double c = std::min(1.0, std::max(-1.0, (tr - 1.0) * 0.5));
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}

bool MatrixToPose(const vr::HmdMatrix34_t& M, float q[4], float p[3]) {
    // A head pose a game hands to Submit (Submit_TextureWithPose) is its own matrix: one with a scale, a shear or a mirror
    // would give a wrong (not just noisy) quaternion below, so it is rejected; small float drift is removed by
    // re-orthonormalizing the columns (Gram-Schmidt: x, then y against x, z = x cross y).
    if (MatrixRotationError(M) > kMaxRotationError) return false;
    double c0[3] = {M.m[0][0], M.m[1][0], M.m[2][0]}, c1[3] = {M.m[0][1], M.m[1][1], M.m[2][1]};
    auto norm = [](double v[3]) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        v[0] /= n; v[1] /= n; v[2] /= n;
    };
    norm(c0);
    const double d = c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2];
    for (int i = 0; i < 3; ++i) c1[i] -= d * c0[i];
    norm(c1);
    const double c2[3] = {c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2], c0[0] * c1[1] - c0[1] * c1[0]};
    const double m00 = c0[0], m01 = c1[0], m02 = c2[0];
    const double m10 = c0[1], m11 = c1[1], m12 = c2[1];
    const double m20 = c0[2], m21 = c1[2], m22 = c2[2];
    double x, y, z, w;
    const double tr = m00 + m11 + m22;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;   // 4w
        w = 0.25 * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;   // 4x
        w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;   // 4y
        w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;   // 4z
        w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s;
    }
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0.5 && n < 1.5) || !std::isfinite(n)) return false;
    q[0] = (float)(x / n); q[1] = (float)(y / n); q[2] = (float)(z / n); q[3] = (float)(w / n);
    p[0] = M.m[0][3]; p[1] = M.m[1][3]; p[2] = M.m[2][3];
    return std::isfinite(p[0] + p[1] + p[2]);
}

vr::HmdMatrix34_t ComposeMatrix(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b) {
    vr::HmdMatrix34_t r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        r.m[i][3] = a.m[i][0] * b.m[0][3] + a.m[i][1] * b.m[1][3] + a.m[i][2] * b.m[2][3] + a.m[i][3];
    }
    return r;
}

void FovFromProjectionRaw(float l, float r, float t, float b, float fov[4]) {
    fov[0] = std::atan(l);
    fov[1] = std::atan(r);
    fov[2] = std::atan(b);   // the y-up top edge: OpenVR's "bottom" (see the header)
    fov[3] = std::atan(t);
}

bool BoundsToRect(const vr::VRTextureBounds_t* b, uint32_t texW, uint32_t texH, int32_t* x, int32_t* y, uint32_t* w,
                  uint32_t* h, bool* flippedU, bool* flippedV) {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (b) { u0 = b->uMin; v0 = b->vMin; u1 = b->uMax; v1 = b->vMax; }
    const bool fu = u0 > u1, fv = v0 > v1;
    if (flippedU) *flippedU = fu;
    if (flippedV) *flippedV = fv;
    if (fu) std::swap(u0, u1);
    if (fv) std::swap(v0, v1);
    if (!std::isfinite(u0) || !std::isfinite(u1) || !std::isfinite(v0) || !std::isfinite(v1)) return false;
    auto clip = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
    u0 = clip(u0); u1 = clip(u1); v0 = clip(v0); v1 = clip(v1);
    const long px0 = std::lround(u0 * texW), px1 = std::lround(u1 * texW);
    const long py0 = std::lround(v0 * texH), py1 = std::lround(v1 * texH);
    if (px1 <= px0 || py1 <= py0) return false;
    *x = (int32_t)px0; *y = (int32_t)py0;
    *w = (uint32_t)(px1 - px0); *h = (uint32_t)(py1 - py0);
    return true;
}

bool SubmitIsSrgb(vr::EColorSpace cs, DXGI_FORMAT f) {
    if (cs == vr::ColorSpace_Gamma) return true;
    if (cs == vr::ColorSpace_Linear) return false;
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM:
        return true;   // 8-bit: gamma
    default:
        return false;  // float / 10-bit: linear
    }
}

DXGI_FORMAT TypedCaptureFormat(DXGI_FORMAT f, bool srgb) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

const char* ColorSpaceName(vr::EColorSpace cs) {
    switch (cs) {
    case vr::ColorSpace_Auto: return "Auto";
    case vr::ColorSpace_Gamma: return "Gamma";
    case vr::ColorSpace_Linear: return "Linear";
    default: return "unknown";
    }
}

const char* TextureTypeName(vr::ETextureType t) {
    switch (t) {
    case vr::TextureType_DirectX: return "DirectX (D3D11)";
    case vr::TextureType_OpenGL: return "OpenGL";
    case vr::TextureType_Vulkan: return "Vulkan";
    case vr::TextureType_IOSurface: return "IOSurface";
    case vr::TextureType_DirectX12: return "DirectX12";
    case vr::TextureType_DXGISharedHandle: return "DXGISharedHandle";
    case vr::TextureType_Metal: return "Metal";
    default: return "unknown";
    }
}

// ------------------------------------------------------------------------------------------------ SteamVR's openvr_api.dll

namespace {

// A minimal JSON reader: enough to walk the top-level object of openvrpaths.vrpath and skip every value it does not need.
struct Json {
    const std::string& s;
    size_t i = 0;
    std::string err;
    explicit Json(const std::string& t) : s(t) {}
    void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; }
    bool Fail(const char* what) { if (err.empty()) err = std::string(what) + " at byte " + std::to_string(i); return false; }
    static int Hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
    bool Str(std::wstring& out) {   // at '"'
        out.clear();
        if (i >= s.size() || s[i] != '"') return Fail("expected a string");
        ++i;
        std::string run;   // plain UTF-8 bytes since the last escape: decoded into `out` before each escape and at the end
        auto flush = [&] { if (!run.empty()) { out += Utf8ToWide(run); run.clear(); } };
        while (i < s.size() && s[i] != '"') {
            const char c = s[i++];
            if ((unsigned char)c < 0x20) return Fail("control character in a string");
            if (c != '\\') { run += c; continue; }
            if (i >= s.size()) return Fail("unterminated escape");
            flush();
            const char e = s[i++];
            switch (e) {
            case '"': out += L'"'; break;
            case '\\': out += L'\\'; break;
            case '/': out += L'/'; break;
            case 'b': out += L'\b'; break;
            case 'f': out += L'\f'; break;
            case 'n': out += L'\n'; break;
            case 'r': out += L'\r'; break;
            case 't': out += L'\t'; break;
            case 'u': {   // one UTF-16 unit; a surrogate pair (two \u escapes) lands as that pair in the wide string
                if (i + 4 > s.size()) return Fail("short \\u escape");
                unsigned v = 0;
                for (int k = 0; k < 4; ++k) {
                    const int h = Hex(s[i + k]);
                    if (h < 0) return Fail("bad \\u escape");
                    v = v * 16 + (unsigned)h;
                }
                i += 4;
                out += (wchar_t)v;
                break;
            }
            default: return Fail("bad escape");
            }
        }
        if (i >= s.size()) return Fail("unterminated string");
        ++i;
        flush();
        return true;
    }
    bool Skip(int depth = 0) {   // any value
        if (depth > 64) return Fail("nested too deep");
        Ws();
        if (i >= s.size()) return Fail("unexpected end");
        const char c = s[i];
        if (c == '"') { std::wstring t; return Str(t); }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i;
            Ws();
            if (i < s.size() && s[i] == close) { ++i; return true; }
            for (;;) {
                if (c == '{') {
                    Ws();
                    std::wstring k;
                    if (!Str(k)) return false;
                    Ws();
                    if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                    ++i;
                }
                if (!Skip(depth + 1)) return false;
                Ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == close) { ++i; return true; }
                return Fail("expected ',' or a closing bracket");
            }
        }
        const size_t st = i;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.')) ++i;
        if (i == st) return Fail("unexpected character");
        const std::string lit = s.substr(st, i - st);
        if (lit == "true" || lit == "false" || lit == "null" || lit[0] == '-' || isdigit((unsigned char)lit[0])) return true;
        return Fail("bad literal");
    }
};

} // namespace

bool ParseOpenvrPathsRuntimes(const std::string& utf8In, std::vector<std::wstring>& out, std::string* error) {
    out.clear();
    std::string text = utf8In;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    Json j(text);
    auto fail = [&](const std::string& why) { if (error) *error = why; out.clear(); return false; };
    j.Ws();
    if (j.i >= text.size() || text[j.i] != '{') return fail("not a JSON object");
    ++j.i;
    bool found = false;
    j.Ws();
    if (j.i < text.size() && text[j.i] == '}') return fail("no \"runtime\" member");
    for (;;) {
        j.Ws();
        std::wstring key;
        if (!j.Str(key)) return fail(j.err);
        j.Ws();
        if (j.i >= text.size() || text[j.i] != ':') return fail("expected ':' at byte " + std::to_string(j.i));
        ++j.i;
        j.Ws();
        if (key == L"runtime" && !found) {
            if (j.i >= text.size() || text[j.i] != '[') return fail("\"runtime\" is not an array");
            ++j.i;
            j.Ws();
            if (j.i < text.size() && text[j.i] == ']') ++j.i;
            else {
                for (;;) {
                    j.Ws();
                    std::wstring v;
                    if (j.i >= text.size() || text[j.i] != '"') return fail("\"runtime\" holds a non-string");
                    if (!j.Str(v)) return fail(j.err);
                    for (auto& c : v) if (c == L'/') c = L'\\';
                    v = StripSlash(Trim(v));
                    if (!v.empty()) out.push_back(v);
                    j.Ws();
                    if (j.i < text.size() && text[j.i] == ',') { ++j.i; continue; }
                    if (j.i < text.size() && text[j.i] == ']') { ++j.i; break; }
                    return fail("bad \"runtime\" array at byte " + std::to_string(j.i));
                }
            }
            found = true;
        } else if (!j.Skip()) {
            return fail(j.err);
        }
        j.Ws();
        if (j.i < text.size() && text[j.i] == ',') { ++j.i; continue; }
        if (j.i < text.size() && text[j.i] == '}') { ++j.i; break; }
        return fail("expected ',' or '}' at byte " + std::to_string(j.i));
    }
    j.Ws();
    if (j.i != text.size()) return fail("text after the JSON object");
    if (!found) return fail("no \"runtime\" member");
    if (error) error->clear();
    return true;
}

std::wstring DefaultOpenvrPathsFile() {
    wchar_t buf[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return StripSlash(buf) + L"\\openvr\\openvrpaths.vrpath";
}

std::wstring FindSteamVrOpenvrApi(const std::wstring& vrpathFile, std::wstring* why) {
    auto no = [&](const std::wstring& w) { if (why) *why = w; return std::wstring(); };
    if (vrpathFile.empty()) return no(L"%LOCALAPPDATA% is not set, so openvrpaths.vrpath cannot be found");
    std::ifstream f(vrpathFile, std::ios::binary);
    if (!f) return no(vrpathFile + L" not found (SteamVR not installed, or never started)");
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<std::wstring> runtimes;
    std::string err;
    if (!ParseOpenvrPathsRuntimes(ss.str(), runtimes, &err)) return no(vrpathFile + L" is not usable: " + Utf8ToWide(err));
    if (runtimes.empty()) return no(vrpathFile + L" lists no SteamVR runtime folder");
    std::wstring tried;
    for (const std::wstring& r : runtimes) {
        const std::wstring dll = r + L"\\bin\\win64\\openvr_api.dll";
        const DWORD a = GetFileAttributesW(dll.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { if (why) why->clear(); return dll; }
        tried += (tried.empty() ? L"" : L", ") + dll;
    }
    return no(L"no openvr_api.dll in the SteamVR runtime folder(s) listed in " + vrpathFile + L" (looked for " + tried + L")");
}
