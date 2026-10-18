// openvr_hooks -- the pure, testable pieces of the SteamVR proxy (tests/openvr_hook_test.cpp links this file alone):
// vtable slot patching, the FnTable copy, raw IVRSystem calls by slot, OpenVR matrix / projection / bounds / colour space
// conversions, and where the proxy finds its home folder (config.ini, logs, captures, the tray).
#pragma once
#include <dxgiformat.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../../third_party/openvr/openvr.h"
#include "openvr_versions.h"

// ------------------------------------------------------------------------------------------------ files / names

// Next to the proxy (the game's openvr_api.dll): the renamed Valve DLL and the sidecar written by the tray's Enable.
#define EYECAP_PROXY_REAL_DLL      L"openvr_api_real.dll"
#define EYECAP_PROXY_SIDECAR       L"eye_capture_proxy.ini"
// Fallback home: HKCU\Software\OpenXR-EyeCapture, REG_SZ value "Home" (written by the tray, non-elevated). Tests point
// the lookup at their own HKCU subkey with XR_EYE_CAPTURE_TEST_HOME_KEY=<subkey> (never the real one).
#define EYECAP_HOME_REG_KEY        L"Software\\OpenXR-EyeCapture"
#define EYECAP_HOME_REG_VALUE      L"Home"
#define EYECAP_TEST_HOME_KEY_ENV   L"XR_EYE_CAPTURE_TEST_HOME_KEY"
// Our own export: present = this openvr_api.dll is the VR EyeCapture proxy (the tray reads the export table).
#define EYECAP_PROXY_VERSION_EXPORT "EyeCaptureProxyVersion"

// ------------------------------------------------------------------------------------------------ home discovery

struct ProxySidecar {
    std::wstring home;   // home = <install dir>
    std::wstring game;   // game = <display name>
};
// Reads `path` (UTF-8, optional BOM; "key = value" lines, ';' / '#' comments, [sections] ignored, quotes stripped).
// false = unreadable or no home key.
bool ReadProxySidecar(const std::wstring& path, ProxySidecar& out);
// Text of a sidecar for these values (UTF-8, CRLF) -- the tray writes exactly this.
std::string ProxySidecarText(const std::wstring& home, const std::wstring& game);

struct ProxyHome {
    std::wstring home;   // folder with config.ini / logs / captures (no trailing slash); empty = none found
    std::wstring game;   // display name from the sidecar (may be empty)
    std::wstring from;   // "sidecar <path>" / "registry HKCU\<key>\Home"
};
// Sidecar next to the proxy first, then the registry value (key from XR_EYE_CAPTURE_TEST_HOME_KEY when set, else
// EYECAP_HOME_REG_KEY). A home must be an existing folder. false = none (the proxy is then a pure forwarder).
bool DiscoverProxyHome(const std::wstring& proxyDir, ProxyHome& out);

// ------------------------------------------------------------------------------------------------ SteamVR's openvr_api.dll

// Quick mode (EyeCaptureMirror) loads SteamVR's OWN openvr_api.dll -- VR EyeCapture ships none. SteamVR writes
// %LOCALAPPDATA%\openvr\openvrpaths.vrpath, a JSON object whose "runtime" member is an array of runtime folders (the first
// one is the active SteamVR); the DLL is <runtime>\bin\win64\openvr_api.dll.
// The "runtime" strings of the top-level JSON object (JSON escapes decoded, '/' turned into '\', no trailing slash).
// false = not a JSON object, no "runtime" member, or not an array of strings (*error says which; out is then empty).
bool ParseOpenvrPathsRuntimes(const std::string& utf8, std::vector<std::wstring>& out, std::string* error = nullptr);
// %LOCALAPPDATA%\openvr\openvrpaths.vrpath ("" when LOCALAPPDATA is not set).
std::wstring DefaultOpenvrPathsFile();
// Reads `vrpathFile`, takes the first runtime whose bin\win64\openvr_api.dll exists and returns that DLL's full path.
// "" + *why (one line: file missing / unreadable / bad JSON / no runtime / no DLL in any of them) otherwise.
std::wstring FindSteamVrOpenvrApi(const std::wstring& vrpathFile, std::wstring* why);

// ------------------------------------------------------------------------------------------------ patching

// Replaces vtable[slot] with `hook` (VirtualProtect around an InterlockedExchangePointer, protection restored; PAGE_READWRITE
// for a data page -- a vtable in .rdata --, PAGE_EXECUTE_READWRITE only when the page was executable). *original
// = the previous entry. false = the page could not be made writable (nothing changed).
bool PatchVtableSlot(void** vtable, uint32_t slot, void* hook, void** original);
// Puts `original` back, but only while the slot still holds `hook` (someone else may have patched over us). false = not
// restored (*current = what the slot holds).
bool RestoreVtableSlot(void** vtable, uint32_t slot, void* hook, void* original, void** current);
// Copies up to n entries of a function table, stopping at the first unreadable entry (SEH). Returns the count copied.
uint32_t CopyFnTable(void* const* src, uint32_t n, void** dst);

// The C++ compositor vtables the proxy patched (two slots each). Read by the hooks WITHOUT a lock: a record is never
// removed and its vtable never changes while its module can be loaded; `restored` = our hooks are no longer in the vtable
// (VR_ShutdownInternal), but a call that had already entered a hook still finds its record (by vtable) and forwards to the
// saved originals. A re-init that hands out the SAME vtable re-arms the existing record (no new record per VR session);
// another vtable gets a new record, or -- table full -- reuses a restored one (its module was unloaded: nothing can call
// through that vtable any more).
struct VtablePatch {
    void** vtable = nullptr;
    int version = -1;                  // the caller's version index (kCompositorVersions)
    uint32_t slotWait = 0, slotSubmit = 0;
    void* volatile origWait = nullptr;
    void* volatile origSubmit = nullptr;
    volatile bool restored = false;
};
class VtablePatchTable {
public:
    static const int kMax = 16;
    enum class Arm { Armed, AlreadyArmed, Rearmed, Full, ProtectFailed };
    // Any record of this vtable (restored or not), nullptr = none. Lock-free.
    const VtablePatch* Find(void** vtable) const;
    // Patches slotWait / slotSubmit of vt with the hooks (or re-arms the restored record of vt). The caller serializes Arm /
    // RestoreAll. *rec (optional) = the record.
    Arm ArmVtable(void** vt, int version, uint32_t slotWait, uint32_t slotSubmit, void* hookWait, void* hookSubmit,
                  const VtablePatch** rec = nullptr);
    // Puts the originals back wherever the slots still hold our hooks (restored = true, the vtable stays in the record).
    // *kept = records whose slots someone else patched over (left as they are). Returns the number restored.
    int RestoreAll(void* hookWait, void* hookSubmit, int* kept = nullptr);
    int Count() const { return n_; }
private:
    VtablePatch rec_[kMax];
    volatile long n_ = 0;
};

// ------------------------------------------------------------------------------------------------ IVRSystem (raw slots)

void SysGetProjectionRaw(void* sys, const SystemVersion& v, vr::EVREye eye, float* l, float* r, float* t, float* b);
vr::HmdMatrix34_t SysGetEyeToHead(void* sys, const SystemVersion& v, vr::EVREye eye);
// Prop_DisplayFrequency_Float of the HMD; false unless the call reports success and 20 < hz < 1000.
bool SysGetDisplayHz(void* sys, const SystemVersion& v, float* hz, int* error);

// ------------------------------------------------------------------------------------------------ conversions

// Row-major 3x4 (rotation | translation) -> unit quaternion (x, y, z, w) + position. false = not a finite rotation.
bool MatrixToPose(const vr::HmdMatrix34_t& m, float q[4], float p[3]);
// a * b (b applied first): eye-to-tracking = head-to-tracking * eye-to-head.
vr::HmdMatrix34_t ComposeMatrix(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b);
// GetProjectionRaw tangents (left < 0 < right, top < 0 < bottom: OpenVR's top is the NEGATIVE tangent) -> OpenXR-style
// fov {angleLeft, angleRight, angleUp, angleDown} = {atan(l), atan(r), atan(-t), atan(-b)}.
void FovFromProjectionRaw(float l, float r, float t, float b, float fov[4]);

// Submit bounds -> pixel rectangle of a texW x texH texture (nullptr = whole texture). u / v outside 0..1 are clipped.
// *flippedU / *flippedV = uMin > uMax / vMin > vMax (the rectangle is then the un-flipped one). false = empty rectangle.
bool BoundsToRect(const vr::VRTextureBounds_t* b, uint32_t texW, uint32_t texH, int32_t* x, int32_t* y, uint32_t* w,
                  uint32_t* h, bool* flippedU, bool* flippedV);

// Is the submitted data sRGB-encoded? Gamma -> yes, Linear -> no, Auto -> an *_SRGB format yes, other 8-bit (UNORM /
// TYPELESS) yes, float / 10-bit no.
bool SubmitIsSrgb(vr::EColorSpace cs, DXGI_FORMAT texFormat);
// The typed format the capture uses for a submitted texture: TYPELESS -> *_UNORM_SRGB (srgb) / *_UNORM (else), 10-bit ->
// R10G10B10A2_UNORM, 16-bit -> R16G16B16A16_FLOAT; typed formats unchanged. (An MSAA resolve needs a typed format.)
DXGI_FORMAT TypedCaptureFormat(DXGI_FORMAT texFormat, bool srgb);
const char* ColorSpaceName(vr::EColorSpace cs);
const char* TextureTypeName(vr::ETextureType t);
