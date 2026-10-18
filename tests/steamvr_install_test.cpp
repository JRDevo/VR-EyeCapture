// steamvr_install_test -- CPU test of tray/steamvr_games.cpp (the tray's "SteamVR games" library) on a fake game tree in
// %TEMP% (never a real game folder): PE / export inspection, game root for an exe, the recursive scan (Unity / UE / 32-bit /
// anti-cheat), enable / idempotent re-enable / disable, "proxy missing" after a simulated Steam update, "original missing",
// OpenComposite detection + restoring SteamVR's DLL from openvr_api.dll.steamvr-backup, a file in use (rolled back, nothing
// changed), and the steamvr_games.ini list. Stand-ins: build\Release\mock_openvr.dll plays Valve's openvr_api.dll, the
// built proxy build\Release\openvr_api.dll is the proxy source. Exit 0 = all passed.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>
#include "steamvr_games.h"
#include "openvr/openvr_hooks.h"

using namespace steamvr;

namespace {

int g_fail = 0, g_pass = 0;
void Check(bool ok, const std::string& what) {
    if (ok) ++g_pass; else { ++g_fail; printf("  FAIL %s\n", what.c_str()); }
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}
void MkDirs(const std::wstring& p) {
    for (size_t i = 3; i <= p.size(); ++i)
        if (i == p.size() || p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
}
std::string ReadBytes(const std::wstring& p) {
    std::string b;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return b;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    b.resize((size_t)sz.QuadPart);
    DWORD r = 0;
    if (!b.empty()) ReadFile(h, &b[0], (DWORD)b.size(), &r, nullptr);
    CloseHandle(h);
    return b;
}
void WriteBytes(const std::wstring& p, const std::string& b) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, b.data(), (DWORD)b.size(), &w, nullptr); CloseHandle(h); }
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring f = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(f);
            else { SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(f.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
std::string N(const std::wstring& w) {   // UTF-8 for the messages
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    for (auto& c : w) if (c == L'/') c = L'\\';
    return w;
}

const Location* Find(const ScanResult& r, const std::wstring& dir) {
    for (const Location& l : r.locations) if (_wcsicmp(l.dir.c_str(), dir.c_str()) == 0) return &l;
    return nullptr;
}

} // namespace

int main() {
    printf("steamvr_install_test\n");
    const std::wstring bin = ExeDir();
    const std::wstring mock = bin + L"\\mock_openvr.dll", proxy = bin + L"\\openvr_api.dll";
    const std::string ver = EYE_CAPTURE_VERSION;
    if (!Exists(mock) || !Exists(proxy)) { printf("FAIL: build mock_openvr and openvr_proxy first (%s)\n", N(bin).c_str()); return 1; }
    const std::string mockBytes = ReadBytes(mock);
    // the game's "Valve" openvr_api.dll in the fake trees: a real Valve DLL -- SteamVR's own copy on this PC (<runtime>\bin\win64
    // from %LOCALAPPDATA%\openvr\openvrpaths.vrpath, FindSteamVrOpenvrApi), or XR_EYE_CAPTURE_TEST_VALVE_DLL. VR EyeCapture ships
    // and keeps no Valve binary, so without SteamVR this test is SKIPPED (exit 0). The DLL is only read and its bytes written into
    // the %TEMP% trees, never loaded. The mock is NOT used there: it has test exports (MockOpenVrCounts ...) the proxy lacks.
    std::wstring valve;
    {
        wchar_t ov[MAX_PATH] = L"";
        const DWORD n = GetEnvironmentVariableW(L"XR_EYE_CAPTURE_TEST_VALVE_DLL", ov, MAX_PATH);
        std::wstring why;
        if (n > 0 && n < MAX_PATH) valve = ov;
        else valve = FindSteamVrOpenvrApi(DefaultOpenvrPathsFile(), &why);
        if (valve.empty()) {
            printf("SKIP: steamvr_install_test needs a real Valve openvr_api.dll; SteamVR's copy was not found (%s). Install SteamVR "
                   "or set XR_EYE_CAPTURE_TEST_VALVE_DLL.\n", N(why).c_str());
            return 0;
        }
        printf("  Valve DLL for the fake game trees: %s\n", N(valve).c_str());
    }
    const std::string gameBytes = ReadBytes(valve);
    if (gameBytes.empty()) { printf("FAIL: cannot read %s\n", N(valve).c_str()); return 1; }

    // ---- InspectDll
    {
        const DllInfo m = InspectDll(mock), p = InspectDll(proxy), none = InspectDll(bin + L"\\no_such.dll");
        Check(m.kind == DllKind::Valve && m.machine == Machine::X64, "mock_openvr.dll = Valve-like x64");
        Check(p.kind == DllKind::Proxy && p.machine == Machine::X64 && p.proxyVersion == ver,
              "proxy = VR EyeCapture proxy x64, version " + p.proxyVersion);
        size_t valveExports = 0;
        for (const char* e : {"LiquidVR", "VRCompositorSystemInternal", "VRControlPanel", "VRDriverDirectInternal", "VRHeadsetView",
                              "VRPaths", "VRRenderModelsInternal", "VRSceneGraph", "VRTrackedCameraInternal", "VRVirtualDisplay",
                              "VRDashboardManager", "VROculusDirect", "VROverlayView",   // older Valve DLLs (CYBRID)
                              "VR_GetGenericInterface", "VR_GetInitToken", "VR_GetRuntimePath", "VR_GetStringForHmdError",
                              "VR_GetVRInitErrorAsEnglishDescription", "VR_GetVRInitErrorAsSymbol", "VR_InitInternal",
                              "VR_InitInternal2", "VR_IsHmdPresent", "VR_IsInterfaceVersionValid", "VR_IsRuntimeInstalled",
                              "VR_RuntimePath", "VR_ShutdownInternal"})
            for (const std::string& x : p.exports) if (x == e) { ++valveExports; break; }
        Check(valveExports == 26 && p.exports.size() == 27, "proxy exports all 26 Valve names (not forwarded) + EyeCaptureProxyVersion (" +
              std::to_string(p.exports.size()) + " exports)");
        Check(none.kind == DllKind::Missing, "missing file");
        const DllInfo v = InspectDll(valve);
        Check(v.kind == DllKind::Valve && v.machine == Machine::X64 && MissingExports(v, p).empty(),
              "Valve's openvr_api.dll (SteamVR's copy): Valve x64, every export covered by the proxy (" + std::to_string(v.exports.size()) + " exports)");
        const std::vector<std::string> miss = MissingExports(m, p);
        Check(miss.size() == 2 && MissingExportsText(miss).find(L"MockOpenVrCounts") != std::wstring::npos,
              "the mock has 2 exports the proxy lacks: " + N(MissingExportsText(miss)));
    }

    // ---- GameRootForExe / GameNameForExe
    Check(GameRootForExe(L"D:\\Games\\Steam\\steamapps\\common\\Crisis VRigade 2\\Crisis VRigade 2.exe") ==
          L"D:\\Games\\Steam\\steamapps\\common\\Crisis VRigade 2", "Steam library: steamapps\\common\\<game>");
    Check(GameRootForExe(L"D:\\Games\\Steam\\steamapps\\common\\Foo\\Foo\\Binaries\\Win64\\Foo-Win64-Shipping.exe") ==
          L"D:\\Games\\Steam\\steamapps\\common\\Foo", "Steam library, UE exe deep inside");
    Check(GameRootForExe(L"E:\\VR\\Bar\\Bar\\Binaries\\Win64\\Bar-Win64-Shipping.exe") == L"E:\\VR\\Bar", "UE layout outside Steam");
    Check(GameRootForExe(L"E:\\VR\\Baz\\Baz.exe") == L"E:\\VR\\Baz", "plain exe folder");
    Check(GameNameForExe(L"E:\\VR\\Bar\\Bar\\Binaries\\Win64\\Bar-Win64-Shipping.exe") == L"Bar", "game name drops -Win64-Shipping");

    // ---- the fake tree
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"eyecap_svtest_" + std::to_wstring(GetCurrentProcessId());
    DeleteTree(root);
    const std::wstring game = root + L"\\steamapps\\common\\Fake Game";
    const std::wstring unityDir = game + L"\\Fake Game_Data\\Plugins\\x86_64";
    const std::wstring ueDir = game + L"\\Engine\\Binaries\\ThirdParty\\OpenVR\\OpenVRv1_5_17\\Win64";
    const std::wstring ue32Dir = game + L"\\Engine\\Binaries\\ThirdParty\\OpenVR\\OpenVRv1_5_17\\Win32";
    const std::wstring home = root + L"\\VR EyeCapture";
    MkDirs(unityDir); MkDirs(ueDir); MkDirs(ue32Dir); MkDirs(home);
    WriteBytes(game + L"\\Fake Game.exe", "MZ not really");
    WriteBytes(unityDir + L"\\openvr_api.dll", gameBytes);
    WriteBytes(ueDir + L"\\openvr_api.dll", gameBytes);
    {   // a "32-bit" copy: the same bytes with IMAGE_FILE_HEADER.Machine = I386
        std::string b = gameBytes;
        DWORD lfanew = 0;
        memcpy(&lfanew, b.data() + 0x3c, 4);
        const WORD m = IMAGE_FILE_MACHINE_I386;
        memcpy(&b[lfanew + 4], &m, 2);
        WriteBytes(ue32Dir + L"\\openvr_api.dll", b);
    }
    Check(GameRootForExe(game + L"\\Fake Game.exe") == game, "game root of the fake tree");

    ScanResult sr = ScanGame(game, ver);
    Check(sr.locations.size() == 3 && !sr.truncated && sr.antiCheat.empty(), "scan: 3 openvr_api.dll folders, no anti-cheat (" +
          std::to_string(sr.locations.size()) + ")");
    const Location* u = Find(sr, unityDir);
    const Location* e = Find(sr, ueDir);
    const Location* x86 = Find(sr, ue32Dir);
    Check(u && u->state == LocState::NotEnabled && u->dll.kind == DllKind::Valve, "Unity Plugins\\x86_64: not enabled, Valve");
    Check(e && e->state == LocState::NotEnabled, "UE Win64: not enabled");
    Check(x86 && x86->state == LocState::Unsupported32 && x86->dll.machine == Machine::X86, "UE Win32: 32-bit, not supported");
    if (!u || !e || !x86) { printf("steamvr_install_test FAIL (scan)\n"); DeleteTree(root); return 1; }

    // ---- enable
    ActionResult r = EnableLocation(*x86, proxy, home, L"Fake Game");
    Check(!r.ok && r.steps.empty() && InspectDll(ue32Dir + L"\\openvr_api.dll").kind == DllKind::Valve, "32-bit refused, untouched");
    for (const Location* L : {u, e}) {
        r = EnableLocation(*L, proxy, home, L"Fake Game");
        Check(r.ok, "enable " + N(L->dir) + ": " + N(r.message));
        const Location after = InspectLocation(L->dir, ver);
        ProxySidecar sc;
        Check(after.state == LocState::Enabled && after.dll.kind == DllKind::Proxy && after.real.kind == DllKind::Valve &&
              !after.proxyOutdated, "enabled: proxy + openvr_api_real.dll (Valve)");
        Check(ReadProxySidecar(L->dir + L"\\" EYECAP_PROXY_SIDECAR, sc) && sc.home == home && sc.game == L"Fake Game", "sidecar home / game");
        Check(!Exists(L->dir + L"\\openvr_api.dll.eyecapture-new"), "no temp file left");
    }
    // idempotent
    {
        const Location now = InspectLocation(unityDir, ver);
        r = EnableLocation(now, proxy, home, L"Fake Game 2");
        const Location after = InspectLocation(unityDir, ver);
        ProxySidecar sc;
        ReadProxySidecar(unityDir + L"\\" EYECAP_PROXY_SIDECAR, sc);
        Check(r.ok && after.state == LocState::Enabled && after.real.kind == DllKind::Valve && sc.game == L"Fake Game 2",
              "enable again: still enabled, the real DLL untouched, sidecar refreshed");
        Check(InspectLocation(unityDir, "9.9.9").proxyOutdated && !InspectLocation(unityDir, "0.0.1").proxyOutdated,
              "an older proxy than ours -> proxyOutdated; a newer one is not");
    }
    // Steam update / verify files: Valve's DLL overwrites the proxy
    {
        std::string updated = gameBytes + "UPDATED";   // still a valid PE (trailing data)
        WriteBytes(unityDir + L"\\openvr_api.dll", updated);
        const Location now = InspectLocation(unityDir, ver);
        Check(now.state == LocState::ProxyMissing, std::string("after a game update: ") + LocStateName(now.state));
        r = EnableLocation(now, proxy, home, L"Fake Game");
        const Location after = InspectLocation(unityDir, ver);
        Check(r.ok && after.state == LocState::Enabled && ReadBytes(unityDir + L"\\" EYECAP_PROXY_REAL_DLL) == updated,
              "enable again after the update: the updated Valve DLL became openvr_api_real.dll");
    }
    // disable
    {
        const Location now = InspectLocation(unityDir, ver);
        r = DisableLocation(now);
        const Location after = InspectLocation(unityDir, ver);
        Check(r.ok && after.state == LocState::NotEnabled && after.dll.kind == DllKind::Valve && after.real.kind == DllKind::Missing &&
              !after.hasSidecar && !Exists(unityDir + L"\\openvr_api.dll.eyecapture-old"),
              "disable: Valve's DLL back, no real / sidecar / leftovers (" + N(r.message) + ")");
        r = DisableLocation(after);
        Check(r.ok && InspectLocation(unityDir, ver).state == LocState::NotEnabled, "disable again: nothing to do");
    }
    // original missing
    {
        DeleteFileW((ueDir + L"\\" EYECAP_PROXY_REAL_DLL).c_str());
        const Location now = InspectLocation(ueDir, ver);
        Check(now.state == LocState::OriginalMissing, std::string("real deleted: ") + LocStateName(now.state));
        Check(!EnableLocation(now, proxy, home, L"Fake Game").ok, "enable refused while the original is missing");
        Check(!DisableLocation(now).ok && InspectLocation(ueDir, ver).dll.kind == DllKind::Proxy, "disable refused without a backup, untouched");
        WriteBytes(ueDir + L"\\openvr_api.dll.steamvr-backup", gameBytes);
        r = DisableLocation(InspectLocation(ueDir, ver));
        Check(r.ok && InspectLocation(ueDir, ver).state == LocState::NotEnabled, "disable restores from .steamvr-backup");
    }
    // a game DLL with an export the proxy lacks: refused before any file operation (Enable and repair alike)
    {
        const std::wstring mx = root + L"\\Mismatch";
        MkDirs(mx);
        WriteBytes(mx + L"\\openvr_api.dll", mockBytes);
        const Location now = InspectLocation(mx, ver);
        r = EnableLocation(now, proxy, home, L"Mismatch");
        Check(now.state == LocState::NotEnabled && !r.ok && r.steps.empty() && r.message.find(L"MockOpenVrCounts") != std::wstring::npos &&
                  r.message.find(L"replacing it could crash the game, so it was not changed") != std::wstring::npos &&
                  ReadBytes(mx + L"\\openvr_api.dll") == mockBytes && !Exists(mx + L"\\" EYECAP_PROXY_REAL_DLL),
              "export mismatch: refused, nothing changed (" + N(r.message) + ")");
        std::vector<GameEntry> g{GameEntry{L"Mismatch", L"", mx, {mx}, L""}};
        WriteBytes(mx + L"\\" EYECAP_PROXY_REAL_DLL, gameBytes);   // looks like "proxy missing" with the mock back as openvr_api.dll
        const std::vector<RepairItem> it = RepairEnabledGames(g, proxy, home, ver);
        Check(it.size() == 1 && it[0].what == RepairItem::What::Failed && it[0].message.find(L"MockOpenVrCounts") != std::wstring::npos &&
                  ReadBytes(mx + L"\\openvr_api.dll") == mockBytes,
              "repair applies the same export check (refused, unchanged)");
    }
    // a running game: its openvr_api.dll is LOADED (LoadLibraryW, like the game). Windows still allows renaming it, so the
    // check must come before any step: Enable and Disable refuse with the in-use text and change nothing.
    {
        const Location now = InspectLocation(unityDir, ver);
        HMODULE lock = LoadLibraryExW((unityDir + L"\\openvr_api.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Check(lock != nullptr && CheckFileAccess(unityDir + L"\\openvr_api.dll") == FileAccess::InUse, "a loaded DLL reads as in use");
        r = EnableLocation(now, proxy, home, L"Fake Game");
        if (lock) FreeLibrary(lock);
        const Location after = InspectLocation(unityDir, ver);
        Check(!r.ok && r.error == ERROR_SHARING_VIOLATION && r.steps.empty() && r.message.find(L"is in use -- close the game") != std::wstring::npos &&
                  after.state == LocState::NotEnabled && after.real.kind == DllKind::Missing &&
                  !Exists(unityDir + L"\\openvr_api.dll.eyecapture-new") && !after.hasSidecar,
              "enable while the game runs: refused (" + N(r.message) + "), nothing changed");
        r = EnableLocation(InspectLocation(unityDir, ver), proxy, home, L"Fake Game");
        Check(r.ok && r.error == 0, "enable after the game closed: ok, no error set");
        HMODULE lock2 = LoadLibraryExW((unityDir + L"\\openvr_api.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // the proxy, loaded
        r = DisableLocation(InspectLocation(unityDir, ver));
        if (lock2) FreeLibrary(lock2);
        const Location a2 = InspectLocation(unityDir, ver);
        Check(lock2 != nullptr && !r.ok && r.steps.empty() && a2.state == LocState::Enabled && !Exists(unityDir + L"\\openvr_api.dll.eyecapture-old"),
              "disable while the game runs: refused before renaming the loaded proxy, still enabled");
        r = DisableLocation(a2);
        Check(r.ok && r.error == 0 && InspectLocation(unityDir, ver).state == LocState::NotEnabled, "disable after the game closed: ok, no error set");
        // leftovers of an interrupted run: removed when free, kept (and reported) while loaded
        WriteBytes(unityDir + L"\\openvr_api.dll.eyecapture-new", "junk");
        const Location l3 = InspectLocation(unityDir, ver);
        Check(l3.staleRemoved.size() == 1 && l3.staleKept.empty() && !Exists(unityDir + L"\\openvr_api.dll.eyecapture-new"), "a stale .eyecapture-new is removed");
        WriteBytes(unityDir + L"\\openvr_api.dll.eyecapture-old", gameBytes);
        HMODULE lock3 = LoadLibraryExW((unityDir + L"\\openvr_api.dll.eyecapture-old").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        const Location l4 = InspectLocation(unityDir, ver);
        Check(lock3 != nullptr && l4.staleKept.size() == 1 && l4.staleRemoved.empty() && Exists(unityDir + L"\\openvr_api.dll.eyecapture-old"),
              "a stale .eyecapture-old that is still loaded is kept and reported");
        if (lock3) FreeLibrary(lock3);
        Check(InspectLocation(unityDir, ver).staleRemoved.size() == 1 && !Exists(unityDir + L"\\openvr_api.dll.eyecapture-old"),
              "... and removed once it is free");
    }

    // ---- OpenComposite
    {
        const std::wstring oc = root + L"\\steamapps\\common\\OC Game\\OC Game_Data\\Plugins\\x86_64";
        MkDirs(oc);
        WriteBytes(oc + L"\\openvr_api.dll", gameBytes + "OpenComposite");   // OpenComposite's DLL names itself
        WriteBytes(oc + L"\\openvr_api.dll.steamvr-backup", gameBytes);
        const Location now = InspectLocation(oc, ver);
        Check(now.dll.kind == DllKind::OpenComposite && now.state == LocState::OpenComposite && now.hasSteamVrBackup, "OpenComposite detected");
        Check(!EnableLocation(now, proxy, home, L"OC Game").ok && InspectLocation(oc, ver).dll.kind == DllKind::OpenComposite,
              "enable refused on OpenComposite, untouched");
        r = RestoreSteamVrDll(now);
        const Location after = InspectLocation(oc, ver);
        Check(r.ok && after.dll.kind == DllKind::Valve && Exists(oc + L"\\openvr_api.dll.opencomposite") && after.state == LocState::NotEnabled,
              "SteamVR's DLL restored from .steamvr-backup, OpenComposite's kept");
        // an OpenComposite switcher shim: exports forwarded to openvr_api_oc.dll, no "OpenComposite" text
        const std::wstring oc2 = root + L"\\OC2";
        MkDirs(oc2);
        const std::string shim = ReadBytes(bin + L"\\oc_shim_test.dll");   // tests/oc_shim_test.def: forwarders only
        WriteBytes(oc2 + L"\\openvr_api.dll", shim);
        const Location sl = InspectLocation(oc2, ver);
        Check(!shim.empty() && sl.dll.kind == DllKind::OpenComposite && sl.state == LocState::OpenComposite,
              "a switcher shim (exports forwarded to openvr_api_oc) -> OpenComposite");
        WriteBytes(oc2 + L"\\openvr_api.dll", "not a PE");
        WriteBytes(oc2 + L"\\openvr_api_oc.dll", gameBytes);
        Check(InspectLocation(oc2, ver).state == LocState::OpenComposite, "openvr_api_oc.dll next to a non-Valve DLL -> OpenComposite");
    }

    // ---- anti-cheat
    {
        MkDirs(game + L"\\EasyAntiCheat");
        const ScanResult s2 = ScanGame(game, ver);
        Check(s2.antiCheat.size() == 1, "EasyAntiCheat folder reported");
    }

    // ---- steamvr_games.ini
    {
        const std::wstring ini = home + L"\\steamvr_games.ini";
        std::vector<GameEntry> list;
        Check(!LoadGameList(ini, list) && list.empty(), "no list yet");
        GameEntry g1{L"Fake Game", game + L"\\Fake Game.exe", game, {unityDir, ueDir}};
        GameEntry g2{L"Sp\u00e9l \u00dc", L"E:\\VR\\Sp\u00e9l\\x.exe", L"E:\\VR\\Sp\u00e9l", {L"E:\\VR\\Sp\u00e9l\\bin"}};
        UpsertGame(list, g1);
        UpsertGame(list, g2);
        g1.dlls.pop_back();
        UpsertGame(list, g1);   // same root: replaced
        Check(list.size() == 2 && list[0].dlls.size() == 1, "upsert by root");
        Check(SaveGameList(ini, list), "save");
        std::vector<GameEntry> back;
        Check(LoadGameList(ini, back) && back.size() == 2 && back[1].name == g2.name && back[1].root == g2.root &&
              back[0].dlls.size() == 1 && back[0].dlls[0] == unityDir && back[0].exe == g1.exe, "load round trip (UTF-8)");
        Check(RemoveGame(back, L"e:\\vr\\sp\u00e9l") && back.size() == 1 && !RemoveGame(back, L"x"), "remove by root (case-insensitive)");
    }

    // ---- versions
    Check(VersionLess("0.4.0", "0.4.1") && VersionLess("0.4.9", "0.4.10") && VersionLess("0.4", "0.4.1") && VersionLess("", "0.1") &&
              !VersionLess("0.5.0", "0.4.9") && !VersionLess("0.4.0", "0.4.0") && !VersionLess("0.4.0", "0.4") && VersionLess("0.4.0-beta", "0.5"),
          "VersionLess: numeric parts, missing = 0, suffixes ignored");

    // ---- Steam's text files (tests/fixtures/steam)
    {
        const std::wstring fx = Widen8(EYE_TEST_FIXTURES) + L"\\steam";
        const std::vector<std::wstring> libs = ParseLibraryFolders(ReadBytes(fx + L"\\libraryfolders.vdf"));
        Check(libs.size() == 3 && libs[0] == L"C:\\Program Files (x86)\\Steam" && libs[1] == L"D:\\SteamLibrary" &&
                  libs[2] == L"E:\\Spiele\\Steam Library",
              "libraryfolders.vdf (new format): 3 paths, escapes undone, '/' -> '\\', trailing slash dropped, d:\\steamlibrary = duplicate");
        const std::vector<std::wstring> old = ParseLibraryFolders(ReadBytes(fx + L"\\libraryfolders_old.vdf"));
        Check(old.size() == 2 && old[0] == L"D:\\SteamLibrary" && old[1] == L"F:\\Games\\Steam",
              "libraryfolders.vdf (old format): numbered values only (TimeNextStatsReport / ContentStatsID ignored)");
        KvNode kv;
        Check(ParseKeyValues(ReadBytes(fx + L"\\libraryfolders.vdf"), kv) && kv.Get(L"LIBRARYFOLDERS") &&
                  kv.Get(L"libraryfolders")->Get(L"1")->Get(L"label")->value == L"Games \"SSD\"",
              "KeyValues: case-insensitive Get, \\\" inside a value");
        AppManifest m;
        Check(ParseAppManifest(ReadBytes(fx + L"\\appmanifest_1612750.acf"), m) && m.appid == L"1612750" && m.name == L"Crisis VRigade 2" &&
                  m.installdir == L"Crisis VRigade 2",
              "appmanifest: appid, name, installdir (nested blocks skipped)");
        Check(!ParseAppManifest(ReadBytes(fx + L"\\appmanifest_bad_installdir.acf"), m), "appmanifest with installdir ..\\..\\Windows refused");
        Check(!ParseAppManifest(std::string("\"AppState\"\n{\n\t\"appid\"\t\"998\"\n\t\"name\"\t\"Trunc"), m), "truncated appmanifest refused");
        Check(ParseLibraryFolders("garbage { {").empty() && ParseLibraryFolders("").empty(), "broken vdf: no libraries");
    }

    // ---- Steam library discovery on a fake Steam install (two libraries)
    const std::wstring steamDir = root + L"\\Steam", lib2 = root + L"\\Lib 2";
    const std::wstring vrOne = steamDir + L"\\steamapps\\common\\VR One", vrOneDll = vrOne + L"\\VR One_Data\\Plugins\\x86_64";
    const std::wstring lib2Game = lib2 + L"\\steamapps\\common\\Lib2Game";
    {
        MkDirs(vrOneDll); MkDirs(lib2Game); MkDirs(steamDir + L"\\steamapps\\common\\Flat One"); MkDirs(steamDir + L"\\steamapps\\common\\Old32\\bin");
        MkDirs(lib2 + L"\\steamapps");
        auto esc = [](std::wstring p) { std::wstring o; for (wchar_t c : p) { if (c == L'\\') o += L"\\\\"; else o += c; } return N(o); };
        WriteBytes(steamDir + L"\\steamapps\\libraryfolders.vdf",
                   "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + esc(steamDir) + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + esc(lib2) +
                       "\"\n\t}\n}\n");
        auto acf = [](const std::wstring& lib, const char* id, const char* name, const char* dir) {
            WriteBytes(lib + L"\\steamapps\\appmanifest_" + Widen8(id) + L".acf",
                       std::string("\"AppState\"\n{\n\t\"appid\"\t\t\"") + id + "\"\n\t\"name\"\t\t\"" + name + "\"\n\t\"installdir\"\t\t\"" + dir + "\"\n}\n");
        };
        acf(steamDir, "101", "VR One", "VR One");
        acf(steamDir, "102", "Flat One", "Flat One");
        acf(steamDir, "103", "Old 32", "Old32");
        acf(steamDir, "104", "Not Installed", "Not Installed");
        acf(lib2, "201", "Lib2 Game", "Lib2Game");
        WriteBytes(vrOneDll + L"\\openvr_api.dll", gameBytes);
        WriteBytes(lib2Game + L"\\openvr_api.dll", gameBytes);
        std::string b32 = gameBytes;
        DWORD lfanew = 0;
        memcpy(&lfanew, b32.data() + 0x3c, 4);
        const WORD i386 = IMAGE_FILE_MACHINE_I386;
        memcpy(&b32[lfanew + 4], &i386, 2);
        WriteBytes(steamDir + L"\\steamapps\\common\\Old32\\bin\\openvr_api.dll", b32);
        const std::vector<std::wstring> libs = SteamLibraries(steamDir);
        Check(libs.size() == 2 && libs[0] == steamDir && libs[1] == lib2, "SteamLibraries: the Steam folder + libraryfolders.vdf, no duplicate");
        SetEnvironmentVariableW(L"EYECAP_TRAY_TEST_STEAM_DIR", nullptr);
        Check(SteamInstallDir(true).empty(), "test override without EYECAP_TRAY_TEST_STEAM_DIR: no Steam (never the real one)");
        SetEnvironmentVariableW(L"EYECAP_TRAY_TEST_STEAM_DIR", steamDir.c_str());
        Check(SteamInstallDir(true) == steamDir, "EYECAP_TRAY_TEST_STEAM_DIR in test mode");
        SetEnvironmentVariableW(L"EYECAP_TRAY_TEST_STEAM_DIR", nullptr);
        const std::vector<FoundGame> found = DiscoverSteamGames(libs, ver);
        for (const FoundGame& f : found) printf("    found %s (%s) %s: %zu x64, %zu x86\n", N(f.name).c_str(), N(f.appid).c_str(), N(f.root).c_str(), f.dlls.size(), f.dlls32.size());
        Check(found.size() == 3 && found[0].name == L"Lib2 Game" && found[1].name == L"Old 32" && found[2].name == L"VR One",
              "discovery: 3 games with an openvr_api.dll, sorted (no DLL / not installed left out)");
        Check(found.size() == 3 && found[0].root == lib2Game && found[0].dlls.size() == 1 && found[1].dlls.empty() && found[1].dlls32.size() == 1 &&
                  found[2].appid == L"101" && found[2].dlls.size() == 1 && found[2].dlls[0] == vrOneDll,
              "discovery: roots / app ids / 64-bit and 32-bit folders");
        volatile bool cancel = true;
        Check(DiscoverSteamGames(libs, ver, &cancel).empty(), "discovery: cancel flag stops it");
        {   // the cheap pre-filter: a deep DLL is found under an Unreal layout (Engine), not in a plain deep folder tree
            const std::wstring ueDeep = lib2 + L"\\steamapps\\common\\Deep UE\\Engine\\Binaries\\ThirdParty\\OpenVR\\OpenVRv1\\Win64";
            const std::wstring plainDeep = lib2 + L"\\steamapps\\common\\Deep Plain\\a\\b\\c\\d\\e";
            MkDirs(ueDeep); MkDirs(plainDeep);
            WriteBytes(ueDeep + L"\\openvr_api.dll", gameBytes);
            WriteBytes(plainDeep + L"\\openvr_api.dll", gameBytes);
            acf(lib2, "202", "Deep UE", "Deep UE");
            acf(lib2, "203", "Deep Plain", "Deep Plain");
            bool hit = true;
            const std::vector<FoundGame> f3 = DiscoverSteamGames(libs, ver, nullptr, &hit);
            bool ue = false, plain = false;
            for (const FoundGame& g : f3) { ue = ue || g.name == L"Deep UE"; plain = plain || g.name == L"Deep Plain"; }
            Check(f3.size() == 4 && ue && !plain && !hit, "pre-filter: Engine layout scanned deep (found), a plain deep tree skipped");
            {   // a library with 400 manifests (games not installed): reading them takes far longer than a 1 ms budget
                const std::wstring big = root + L"\\BigLib";
                MkDirs(big + L"\\steamapps");
                for (int i = 0; i < 400; ++i) acf(big, std::to_string(5000 + i).c_str(), "Not There", "Not There");
                hit = false;
                DiscoverSteamGames(std::vector<std::wstring>{big}, ver, nullptr, &hit, 1);
                Check(hit, "total budget (1 ms, 400 manifests): the scan stops early and says so");
                hit = true;
                DiscoverSteamGames(std::vector<std::wstring>{big}, ver, nullptr, &hit, 0);
                Check(!hit, "no budget (0): the whole library is read");
            }
            DeleteTree(lib2 + L"\\steamapps\\common\\Deep UE");
            DeleteTree(lib2 + L"\\steamapps\\common\\Deep Plain");
            DeleteFileW((lib2 + L"\\steamapps\\appmanifest_202.acf").c_str());
            DeleteFileW((lib2 + L"\\steamapps\\appmanifest_203.acf").c_str());
        }
        // the cache next to the [game] entries
        const std::wstring ini = home + L"\\svr_cache.ini";
        std::vector<GameEntry> games{GameEntry{L"VR One", L"", vrOne, {vrOneDll}, L"101"}};
        Check(SaveGameList(ini, games, &found), "save [game] + [found]");
        std::vector<GameEntry> g2;
        std::vector<FoundGame> f2;
        Check(LoadGameList(ini, g2) && g2.size() == 1 && g2[0].appid == L"101" && g2[0].dlls.size() == 1 && LoadFoundGames(ini, f2) &&
                  f2.size() == 3 && f2[1].dlls32.size() == 1 && f2[2].root == vrOne,
              "round trip: [found] does not leak into the games, appid kept");
        Check(SaveGameList(ini, std::vector<GameEntry>{}) && LoadFoundGames(ini, f2) && f2.size() == 3 && LoadGameList(ini, g2) && g2.empty(),
              "SaveGameList without `found` keeps the cache");
        DeleteFileW(ini.c_str());
    }

    // ---- automatic repair (only the games the user enabled)
    {
        Check(EnableLocation(InspectLocation(vrOneDll, ver), proxy, home, L"VR One").ok, "VR One enabled");
        std::vector<GameEntry> games{GameEntry{L"VR One", L"", vrOne, {vrOneDll}, L"101"}};
        const std::string proxyBytes = ReadBytes(proxy);
        Check(RepairEnabledGames(games, proxy, home, ver).empty(), "nothing to repair while enabled");
        // a game update: Valve's DLL over the proxy, in VR One (enabled) and in Lib2Game (NOT enabled, made to look the same)
        WriteBytes(vrOneDll + L"\\openvr_api.dll", gameBytes + "V2");
        WriteBytes(lib2Game + L"\\openvr_api_real.dll", gameBytes);
        WriteBytes(lib2Game + L"\\" EYECAP_PROXY_SIDECAR, "home = x\r\n");
        Check(InspectLocation(vrOneDll, ver).state == LocState::ProxyMissing && InspectLocation(lib2Game, ver).state == LocState::ProxyMissing,
              "both folders look updated (proxy missing)");
        {   // the game runs: its openvr_api.dll is open (here: a handle without sharing) -> skipped, unchanged
            HMODULE h = LoadLibraryExW((vrOneDll + L"\\openvr_api.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            Check(h != nullptr && FileInUse(vrOneDll + L"\\openvr_api.dll"), "FileInUse: the game has the (updated) DLL loaded");
            const std::vector<RepairItem> it = RepairEnabledGames(games, proxy, home, ver);
            if (h) FreeLibrary(h);
            Check(it.size() == 1 && it[0].what == RepairItem::What::SkippedInUse && ReadBytes(vrOneDll + L"\\openvr_api.dll") == gameBytes + "V2" &&
                      !Exists(vrOneDll + L"\\openvr_api.dll.eyecapture-new"),
                  "game running: skipped, nothing changed (retried next start)");
        }
        Check(!FileInUse(vrOneDll + L"\\openvr_api.dll") && !FileInUse(vrOneDll + L"\\no_such.dll"), "FileInUse: closed / missing file");
        const std::vector<RepairItem> it = RepairEnabledGames(games, proxy, home, ver);
        Check(it.size() == 1 && it[0].what == RepairItem::What::Repaired && it[0].game == L"VR One", "repaired: one item");
        Check(ReadBytes(vrOneDll + L"\\openvr_api.dll") == proxyBytes && ReadBytes(vrOneDll + L"\\openvr_api_real.dll") == gameBytes + "V2" &&
                  InspectLocation(vrOneDll, ver).state == LocState::Enabled,
              "repaired: the proxy is back, the UPDATED Valve DLL is the new openvr_api_real.dll");
        Check(ReadBytes(lib2Game + L"\\openvr_api.dll") == gameBytes && !Exists(lib2Game + L"\\openvr_api.dll.eyecapture-new") &&
                  InspectLocation(lib2Game, ver).state == LocState::ProxyMissing,
              "a game the user did not enable is never touched");
        // an outdated proxy (older version marker) is refreshed the same way
        std::string oldProxy = proxyBytes;
        const size_t mk = oldProxy.find("EYECAP_PROXY_VERSION=");
        Check(mk != std::string::npos && ver.size() == 5, "proxy version marker found");
        if (mk != std::string::npos && ver.size() == 5) memcpy(&oldProxy[mk + 21], "0.0.1", 5);
        WriteBytes(vrOneDll + L"\\openvr_api.dll", oldProxy);
        Check(InspectDll(vrOneDll + L"\\openvr_api.dll").proxyVersion == "0.0.1" && InspectLocation(vrOneDll, ver).proxyOutdated,
              "an older proxy -> proxyOutdated");
        const std::vector<RepairItem> up = RepairEnabledGames(games, proxy, home, ver);
        Check(up.size() == 1 && up[0].what == RepairItem::What::Updated && ReadBytes(vrOneDll + L"\\openvr_api.dll") == proxyBytes &&
                  ReadBytes(vrOneDll + L"\\openvr_api_real.dll") == gameBytes + "V2",
              "outdated proxy updated, the real DLL untouched");
        Check(RepairEnabledGames(games, proxy, home, ver).empty(), "nothing left to do");

        // FindRepairs (what the tray runs at start): read-only, reports what RepairEnabledGames would act on
        Check(FindRepairs(games, home, ver).empty(), "FindRepairs: nothing while enabled and up to date");
        WriteBytes(vrOneDll + L"\\openvr_api.dll", gameBytes + "V3");   // another game update
        {
            const std::vector<RepairNeed> f = FindRepairs(games, home, ver);
            Check(f.size() == 1 && f[0].why == RepairWhy::GameUpdated && f[0].game == L"VR One" && f[0].dir == vrOneDll &&
                      ReadBytes(vrOneDll + L"\\openvr_api.dll") == gameBytes + "V3" && !Exists(vrOneDll + L"\\openvr_api.dll.eyecapture-new") &&
                      InspectLocation(vrOneDll, ver).state == LocState::ProxyMissing,
                  "FindRepairs: the update is reported (game updated) and nothing is changed");
            Check(InspectLocation(lib2Game, ver).state == LocState::ProxyMissing && FindRepairs(games, home, ver).size() == 1,
                  "FindRepairs: a game the user did not enable is not reported");
        }
        Check(RepairEnabledGames(games, proxy, home, ver).size() == 1 && InspectLocation(vrOneDll, ver).state == LocState::Enabled,
              "repaired on the click");

        // VR EyeCapture moved: the sidecar's home is a folder that no longer exists / another existing folder
        const std::wstring side = vrOneDll + L"\\" EYECAP_PROXY_SIDECAR;
        WriteBytes(side, "home = " + N(root) + "\\old install\r\ngame = VR One\r\n");
        {
            const Location L = InspectLocation(vrOneDll, ver);
            const std::vector<RepairNeed> f = FindRepairs(games, home, ver);
            Check(L.state == LocState::Enabled && RepairNeeded(L, home) == RepairWhy::Moved && RepairNeeded(L, L"") == RepairWhy::None &&
                      f.size() == 1 && f[0].why == RepairWhy::Moved && std::string(RepairWhyName(f[0].why)) == "VR EyeCapture moved",
                  "sidecar home gone: Needs repair (VR EyeCapture moved); the check is skipped without a home");
        }
        MkDirs(root + L"\\other install");
        WriteBytes(side, "home = " + N(root) + "\\other install\r\ngame = VR One\r\n");
        Check(RepairNeeded(InspectLocation(vrOneDll, ver), home) == RepairWhy::Moved, "sidecar home = another existing folder: Moved");
        {
            std::wstring upper = home;
            for (auto& c : upper) c = (wchar_t)towupper(c);
            WriteBytes(side, "home = " + N(upper) + "\\\r\ngame = VR One\r\n");
            Check(RepairNeeded(InspectLocation(vrOneDll, ver), home) == RepairWhy::None, "same folder with another case / a trailing slash: not moved");
        }
        WriteBytes(side, "home = " + N(root) + "\\old install\r\ngame = VR One\r\n");
        {
            const std::vector<RepairItem> it = RepairEnabledGames(games, proxy, home, ver);
            ProxySidecar sc;
            Check(it.size() == 1 && it[0].what == RepairItem::What::Moved && ReadProxySidecar(side, sc) && sc.home == home &&
                      ReadBytes(vrOneDll + L"\\openvr_api.dll") == proxyBytes && ReadBytes(vrOneDll + L"\\openvr_api_real.dll") == gameBytes + "V3",
                  "moved: repaired on the click -- sidecar home = this install, proxy and real DLL kept");
            Check(FindRepairs(games, home, ver).empty(), "moved: nothing left to repair");
        }
    }

    DeleteTree(root);
    printf("steamvr_install_test: %d passed, %d failed -> %s\n", g_pass, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
