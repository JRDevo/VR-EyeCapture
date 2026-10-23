// EyeCaptureVrInput.exe -- the VR controller hotkey (config vr_hotkey / vr_hotkey_hold_s) for SteamVR games. The OpenXR layer
// reads the controllers inside an OpenXR game (src/vr_input.cpp); a SteamVR game registers its own SteamVR Input action
// manifest and SteamVR takes one per process, so the proxy cannot add a button there (src/openvr/vrinput.h). This helper is
// its own small SteamVR overlay application instead:
//
//   * The tray starts it while a SteamVR game recorded by VR EyeCapture is live (status block api "SteamVR ...": the proxy
//     or the quick-mode mirror) and vr_hotkey is not off: EyeCaptureVrInput.exe --home <dir> --game-pid <pid>. It exits
//     when that process ends, on VREvent_Quit, on Local\OpenXR-EyeCapture-VrInputQuit, or when the tray is gone
//     (require_tray). Single instance (Local\OpenXR-EyeCapture-VrInput; exit code 3).
//   * Loads SteamVR's OWN openvr_api.dll (like EyeCaptureMirror.exe; XR_EYE_CAPTURE_VRINPUT_OPENVR_DLL = tests: the mock) and
//     VR_InitInternal2(VRApplication_Overlay) -- an overlay application gets controller input while a game has the focus
//     (the way OVR Advanced Settings' push-to-talk works); it never starts SteamVR (only started while a SteamVR game runs).
//   * Writes the action manifest + default bindings (src/openvr/vrinput.cpp) to <user data>\steamvr_input\ and hands the
//     manifest to IVRInput_010 (C++ slots, Valve's openvr.h 1.16: 0 SetActionManifestPath, 1 GetActionSetHandle,
//     2 GetActionHandle, 4 UpdateActionState, 5 GetDigitalActionData, 21 TriggerHapticVibrationAction). The action set
//     runs at priority 0: the game still gets the same buttons (no overlay-priority override).
//   * 50 times a second: UpdateActionState + each button's digital action; all held for vr_hotkey_hold_s (VrHoldTracker)
//     -> ECC_TOGGLE through the status block (the tray's --toggle path) + a short vibration on both hands.
//   * Logs to <home>\logs\eye_capture.log ("vr input: ..."): the files, the handles, whether SteamVR delivers the actions
//     (bActive) and every toggle.
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <vector>

#include "capture_common.h"
#include "config.h"
#include "log.h"
#include "openvr_hooks.h"
#include "shared_status.h"
#include "vrinput.h"

namespace {

const wchar_t kQuitEventName[] = L"Local\\OpenXR-EyeCapture-VrInputQuit";
const wchar_t kSingleName[] = L"Local\\OpenXR-EyeCapture-VrInput";
const int kExitBusy = 3;
const int kExitNoSteamVr = 4;
const char kInputVersion[] = "IVRInput_010";
const char kSystemVersion[] = "IVRSystem_022";

// IVRInput_010 types (Valve's openvr.h; pack 8)
typedef uint64_t VRActionHandle;
typedef uint64_t VRActionSetHandle;
typedef uint64_t VRInputValueHandle;
struct ActiveActionSet {
    VRActionSetHandle  set = 0;
    VRInputValueHandle restrictedToDevice = 0;
    VRActionSetHandle  secondarySet = 0;
    uint32_t           padding = 0;
    int32_t            priority = 0;
};
struct DigitalActionData {
    bool               active = false;
    VRInputValueHandle activeOrigin = 0;
    bool               state = false;
    bool               changed = false;
    float              updateTime = 0;
};
static_assert(sizeof(ActiveActionSet) == 32 && sizeof(DigitalActionData) == 24, "IVRInput struct layout");

typedef int (*PFN_SetManifest)(void*, const char*);
typedef int (*PFN_GetHandle)(void*, const char*, uint64_t*);
typedef int (*PFN_UpdateState)(void*, ActiveActionSet*, uint32_t, uint32_t);
typedef int (*PFN_GetDigital)(void*, VRActionHandle, DigitalActionData*, uint32_t, VRInputValueHandle);
typedef int (*PFN_Haptic)(void*, VRActionHandle, float, float, float, float, VRInputValueHandle);

struct Input {
    void* obj = nullptr;
    void* const* vt = nullptr;
    template <class F> F Slot(int i) const { return reinterpret_cast<F>(vt[i]); }
};

std::string N(const std::wstring& w) { return elog::Narrow(w); }

std::wstring ExeDir() {
    wchar_t p[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

// --name <value> (quoted or not)
std::wstring Arg(const std::wstring& args, const wchar_t* name) {
    const std::wstring key = std::wstring(name) + L" ";
    const size_t k = args.find(key);
    if (k == std::wstring::npos) return std::wstring();
    std::wstring v = args.substr(k + key.size());
    while (!v.empty() && v.front() == L' ') v.erase(0, 1);
    if (!v.empty() && v.front() == L'"') { const size_t q = v.find(L'"', 1); v = v.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1); }
    else { const size_t sp = v.find(L' '); if (sp != std::wstring::npos) v = v.substr(0, sp); }
    while (!v.empty() && (v.back() == L'\\' || v.back() == L'/')) v.pop_back();
    return v;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &n, nullptr) && n == text.size();
    CloseHandle(f);
    return ok;
}

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// The manifest + one binding file per controller type into dir. Returns the manifest path ("" = failed, logged).
std::wstring WriteInputFiles(const std::wstring& dir, const VrHotkey& hk) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring manifest = dir + L"\\actions.json";
    if (!WriteFileUtf8(manifest, SteamVrManifestJson(hk))) {
        LOGE("vr input: cannot write %s (error %lu)", N(manifest).c_str(), GetLastError());
        return std::wstring();
    }
    std::string bound;
    for (const char* t : SteamVrControllerTypes()) {
        int n = 0;
        const std::string json = SteamVrBindingJson(t, hk, &n);
        const std::wstring file = dir + L"\\bindings_" + std::wstring(t, t + strlen(t)) + L".json";
        if (!WriteFileUtf8(file, json)) { LOGE("vr input: cannot write %s", N(file).c_str()); return std::wstring(); }
        char part[96];
        snprintf(part, sizeof(part), "%s%s %d/%d", bound.empty() ? "" : ", ", t, n, (int)hk.buttons.size());
        bound += part;
    }
    LOGI("vr input: action manifest %s (buttons bound per controller: %s)", N(manifest).c_str(), bound.c_str());
    return manifest;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, kSingleName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return kExitBusy;
    const std::wstring args = cmdLine ? cmdLine : L"";
    std::wstring home = Arg(args, L"--home");
    if (home.empty()) home = ExeDir();
    const DWORD gamePid = (DWORD)_wtoi(Arg(args, L"--game-pid").c_str());
    elog::Init(LogsDir(home) + L"\\eye_capture.log", false);
    Config cfg;
    LoadConfig(home, cfg);
    const CaptureEnv env = ReadCaptureEnv();
    if (!env.configOverrides.empty()) ApplyConfigOverrides(env.configOverrides, cfg);
    if (!env.requireTray.empty()) cfg.requireTray = _wtoi(env.requireTray.c_str()) != 0;
    elog::SetLevel(cfg.logLevel);
    LOGI("==== VR EyeCapture SteamVR controller hotkey v%s (pid %lu) for game pid %lu | vr_hotkey %s, hold %.1f s", EYE_CAPTURE_VERSION,
         (unsigned long)GetCurrentProcessId(), (unsigned long)gamePid, N(cfg.vrHotkey.text).c_str(), cfg.vrHotkeyHoldS);
    if (!cfg.vrHotkey.enabled || cfg.vrHotkey.buttons.empty()) { LOGI("vr input: vr_hotkey = off -- nothing to do"); return 0; }

    // SteamVR's own openvr_api.dll
    std::wstring dllPath = EnvW(L"XR_EYE_CAPTURE_VRINPUT_OPENVR_DLL");
    if (!dllPath.empty()) LOGI("XR_EYE_CAPTURE_VRINPUT_OPENVR_DLL=%s (test override)", N(dllPath).c_str());
    else {
        std::wstring vrpath = EnvW(L"XR_EYE_CAPTURE_TEST_OPENVRPATHS");
        if (vrpath.empty()) vrpath = DefaultOpenvrPathsFile();
        std::wstring why;
        dllPath = FindSteamVrOpenvrApi(vrpath, &why);
        if (dllPath.empty()) { LOGE("vr input: SteamVR's openvr_api.dll not found -- %s", N(why).c_str()); return kExitNoSteamVr; }
    }
    HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { LOGE("vr input: cannot load %s (error %lu)", N(dllPath).c_str(), GetLastError()); return 1; }
    if (GetProcAddress(dll, EYECAP_PROXY_VERSION_EXPORT)) { LOGE("vr input: %s is the VR EyeCapture proxy -- refused", N(dllPath).c_str()); return 1; }
    auto init2 = (PFN_VR_InitInternal2)GetProcAddress(dll, "VR_InitInternal2");
    auto shutdown = (PFN_VR_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal");
    auto getIf = (PFN_VR_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface");
    if (!init2 || !shutdown || !getIf) { LOGE("vr input: %s lacks the VR_* exports", N(dllPath).c_str()); return 1; }

    vr::EVRInitError ie = vr::VRInitError_None;
    init2(&ie, vr::VRApplication_Overlay, nullptr);
    if (ie != vr::VRInitError_None) { LOGE("vr input: VR_InitInternal2(Overlay) -> error %d", (int)ie); return 1; }
    vr::EVRInitError e1 = vr::VRInitError_None, e2 = vr::VRInitError_None;
    auto* sys = static_cast<vr::IVRSystem*>(getIf(kSystemVersion, &e1));
    Input in;
    in.obj = getIf(kInputVersion, &e2);
    in.vt = in.obj ? *static_cast<void* const* const*>(in.obj) : nullptr;
    LOGI("vr input: VR_InitInternal2(Overlay) OK | %s -> %p (error %d) | %s -> %p (error %d)", kSystemVersion, (void*)sys, (int)e1,
         kInputVersion, in.obj, (int)e2);
    int rc = 0;
    VRActionSetHandle set = 0;
    VRActionHandle haptic = 0;
    std::vector<VRActionHandle> acts(cfg.vrHotkey.buttons.size(), 0);
    if (!in.vt) { LOGE("vr input: the runtime does not offer %s -- no controller hotkey", kInputVersion); rc = 1; }
    if (rc == 0) {
        const std::wstring manifest = WriteInputFiles(UserDataDir(home) + L"\\steamvr_input", cfg.vrHotkey);
        if (manifest.empty()) rc = 1;
        else {
            const int me = in.Slot<PFN_SetManifest>(0)(in.obj, Utf8(manifest).c_str());
            const int se = in.Slot<PFN_GetHandle>(1)(in.obj, kSteamVrActionSet, &set);
            const int he = in.Slot<PFN_GetHandle>(2)(in.obj, kSteamVrHapticAction, &haptic);
            std::string ah;
            for (size_t i = 0; i < acts.size(); ++i) {
                const int ae = in.Slot<PFN_GetHandle>(2)(in.obj, SteamVrButtonAction((int)i).c_str(), &acts[i]);
                char part[160];
                snprintf(part, sizeof(part), "%s%s = %s %llx (error %d)", i ? ", " : "", SteamVrButtonAction((int)i).c_str(),
                         VrButtonName(cfg.vrHotkey.buttons[i]).c_str(), (unsigned long long)acts[i], ae);
                ah += part;
            }
            LOGI("vr input: SetActionManifestPath -> error %d | set %llx (error %d), haptic %llx (error %d) | %s", me,
                 (unsigned long long)set, se, (unsigned long long)haptic, he, ah.c_str());
            if (me != 0 || !set) { LOGE("vr input: SteamVR refused the action manifest -- no controller hotkey"); rc = 1; }
        }
    }

    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
    HANDLE game = gamePid ? OpenProcess(SYNCHRONIZE, FALSE, gamePid) : nullptr;
    HANDLE map = nullptr;
    const std::wstring testShm = EnvW(L"XR_EYE_CAPTURE_TEST_SHM");
    EyeCapShared* shm = testShm.empty() ? EyeCapOpenShared(&map) : EyeCapOpenShared(&map, testShm.c_str());
    VrHoldTracker hold;
    uint64_t polls = 0, activePolls = 0, toggles = 0;
    bool loggedActive = false, loggedInactive = false;
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG lastCheck = 0;
    while (rc == 0) {
        Sleep(20);
        const ULONGLONG now = GetTickCount64();
        if (now - lastCheck >= 500) {
            lastCheck = now;
            if (quit && WaitForSingleObject(quit, 0) == WAIT_OBJECT_0) { LOGI("vr input: quit requested by the tray"); break; }
            if (game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0) { LOGI("vr input: the game (pid %lu) ended", (unsigned long)gamePid); break; }
            if (cfg.requireTray && !EyeCapTrayRunning()) { LOGI("vr input: EyeCaptureTray is not running (require_tray) -- exiting"); break; }
            if (sys) {
                vr::VREvent_t ev{};
                bool q = false;
                for (int i = 0; i < 64 && sys->PollNextEvent(&ev, sizeof(ev)); ++i) if (ev.eventType == vr::VREvent_Quit) q = true;
                if (q) { LOGI("vr input: VREvent_Quit -- SteamVR is closing"); break; }
            }
        }
        ActiveActionSet as;
        as.set = set;
        const int ue = in.Slot<PFN_UpdateState>(4)(in.obj, &as, (uint32_t)sizeof(as), 1);
        bool all = ue == 0, anyActive = false;
        for (VRActionHandle a : acts) {
            DigitalActionData d;
            const int de = in.Slot<PFN_GetDigital>(5)(in.obj, a, &d, (uint32_t)sizeof(d), 0);
            if (de == 0 && d.active) anyActive = true;
            if (de != 0 || !d.active || !d.state) all = false;
        }
        ++polls;
        if (anyActive) ++activePolls;
        if (anyActive && !loggedActive) {
            loggedActive = true;
            LOGI("vr input: SteamVR delivers the hotkey actions (after %.1f s)", (now - t0) / 1000.0);
        } else if (!anyActive && !loggedActive && !loggedInactive && now - t0 > 10000) {
            loggedInactive = true;
            LOGW("vr input: no hotkey action is active after 10 s (UpdateActionState error %d) -- SteamVR gives this app no "
                 "controller input or no binding for this controller (SteamVR -> Controller Bindings -> VR EyeCapture)", ue);
        }
        if (hold.Update(all, now / 1000.0, cfg.vrHotkeyHoldS)) {
            ++toggles;
            const bool sent = shm && EyeCapSendCommand(shm, ECC_TOGGLE);
            LOGI("vr input: %s held %.1f s -> %s", N(cfg.vrHotkey.text).c_str(), cfg.vrHotkeyHoldS,
                 sent ? "toggle sent" : "no game is running (nothing sent)");
            if (sent && haptic) in.Slot<PFN_Haptic>(21)(in.obj, haptic, 0.0f, 0.1f, 160.0f, 0.6f, 0);
        }
    }
    LOGI("vr input: %llu polls (%llu with an active action), %llu toggles; shutting down", (unsigned long long)polls,
         (unsigned long long)activePolls, (unsigned long long)toggles);
    shutdown();
    if (shm) UnmapViewOfFile(shm);
    if (map) CloseHandle(map);
    if (game) CloseHandle(game);
    if (quit) CloseHandle(quit);
    if (single) CloseHandle(single);
    return rc;
}
