// steamvr_ui -- see steamvr_ui.h.
#include "steamvr_ui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "config.h"
#include "glass.h"
#include "log.h"
#include "openvr/openvr_hooks.h"   // EYECAP_HOME_REG_KEY / _VALUE, EYECAP_TEST_HOME_KEY_ENV
#include "steamvr_games.h"

namespace steamvr_ui {

namespace {

using steamvr::FoundGame;
using steamvr::GameEntry;
using steamvr::Location;
using steamvr::LocState;

// layout (client pixels; the tray is not DPI-aware, Windows scales the window)
const int kClientW = 860, kClientH = 572, kMargin = 12;
const int kCheckW = 22;                                       // the check box column of the list
const int kColState = kCheckW + 230, kColFolder = kCheckW + 470;   // text columns, relative to the list's left edge
const UINT WM_SVR_SCANDONE = WM_APP + 40;

Options     g_opt;
HWND        g_dlg = nullptr;
bool        g_done = false;
HFONT       g_font = nullptr;
std::string g_ourVersion;   // version of the proxy we would install ("" = no proxy found)
std::wstring g_status;      // the status line (scan / repair)

// steamvr_games.ini is written by the UI thread and the scan thread: one lock around every read-modify-write.
// Heap objects never freed: the detached scan thread may outlive the window (and wWinMain's statics).
std::mutex& g_iniM = *new std::mutex;
struct ScanShared {
    std::mutex m;
    bool running = false;
    volatile bool cancel = false;
    HWND notify = nullptr;          // window to post WM_SVR_SCANDONE to (nullptr = closed)
    std::wstring summary;           // "Steam library: 3 SteamVR games in 2 libraries" / "Steam not found"
};
ScanShared& g_scan = *new ScanShared;

enum class RowState { Off, On, NeedsRepair, OpenComposite, Only32, OriginalMissing, NotFound, Unknown };
struct Row {
    std::wstring name, root, exe, appid;
    bool enabled = false;               // a [game] entry exists (the user enabled it)
    bool steam = false;                 // in the Steam scan cache
    bool antiCheat = false;
    bool truncated = false;             // the Steam scan stopped early in this folder (very large): it may have more
    std::vector<std::wstring> dirs;     // 64-bit DLL folders (entry dlls + found dlls)
    std::vector<std::wstring> dirs32;
    std::vector<Location> locs;         // dirs, inspected
    RowState state = RowState::Unknown;
    steamvr::RepairWhy repairWhy = steamvr::RepairWhy::None;   // NeedsRepair: why (the first folder that needs one)
};
std::vector<Row> g_rows;

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
// The list lives with the user's data: next to the app in a portable / development folder, %LOCALAPPDATA%\VR EyeCapture when
// installed (config.h UserDataDir). `dir` = the install folder.
std::wstring IniPathFor(const std::wstring& dir) { return UserDataDir(dir) + L"\\steamvr_games.ini"; }
std::wstring IniPath() { return IniPathFor(g_opt.installDir); }
std::string N(const std::wstring& w) { return elog::Narrow(w); }
bool SameDir(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
void AddUnique(std::vector<std::wstring>& v, const std::wstring& d) {
    for (const std::wstring& x : v) if (SameDir(x, d)) return;
    v.push_back(d);
}

// `dir` relative to `root` ("(game folder)" when equal), else the full path.
std::wstring Rel(const std::wstring& root, const std::wstring& dir) {
    if (SameDir(root, dir)) return L"(game folder)";
    if (dir.size() > root.size() + 1 && _wcsnicmp(dir.c_str(), root.c_str(), root.size()) == 0 && dir[root.size()] == L'\\')
        return dir.substr(root.size() + 1);
    return dir;
}

// A result line of steamvr_games ("in use -- close the game ...") as a sentence for a message box: capital first letter,
// " -- " as ". ", a full stop at the end.
std::wstring Sentence(std::wstring s) {
    for (size_t p; (p = s.find(L" -- ")) != std::wstring::npos;) {
        s.replace(p, 4, L". ");
        if (p + 2 < s.size()) s[p + 2] = (wchar_t)towupper(s[p + 2]);
    }
    if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
    if (!s.empty() && s.back() != L'.' && s.back() != L')') s += L'.';
    return s;
}

const wchar_t* StateText(RowState s, steamvr::RepairWhy why = steamvr::RepairWhy::GameUpdated) {
    switch (s) {
    case RowState::On: return L"On";
    case RowState::Off: return L"Off";
    case RowState::NeedsRepair:
        return why == steamvr::RepairWhy::Moved ? L"Needs repair (VR EyeCapture moved) — click Repair"
             : why == steamvr::RepairWhy::ProxyOutdated ? L"Needs repair (older recorder version) — click Repair"
             : L"Needs repair (game updated) — click Repair";
    case RowState::OpenComposite: return L"OpenComposite installed";
    case RowState::Only32: return L"32-bit, not supported";
    case RowState::OriginalMissing: return L"original missing — verify the game files in Steam";
    case RowState::NotFound: return L"folder not found — uninstalled or moved?";
    default: return L"unknown openvr_api.dll — not changed";
    }
}
std::wstring LocText(const Location& L) {
    switch (L.state) {
    case LocState::Enabled:
        if (steamvr::RepairNeeded(L, g_opt.installDir) == steamvr::RepairWhy::Moved)
            return L"on, but its settings point to " + (L.sidecarHome.empty() ? std::wstring(L"(nothing)") : L.sidecarHome) +
                   L" -- VR EyeCapture moved? Repair";
        return L.proxyOutdated ? L"on (older proxy)" : L"on";
    case LocState::ProxyMissing: return L"proxy missing — game updated?";
    case LocState::OriginalMissing: return L"original missing";
    case LocState::OpenComposite: return L"OpenComposite installed";
    case LocState::NotEnabled: return L"off";
    case LocState::Unsupported32: return L"32-bit — not supported";
    default: return L"unknown openvr_api.dll";
    }
}

RowState StateOf(Row& r) {
    r.repairWhy = steamvr::RepairWhy::None;
    if (!DirExists(r.root)) return RowState::NotFound;
    bool on = false, repair = false, orig = false, oc = false, off = false;
    for (const Location& L : r.locs) {
        const steamvr::RepairWhy w = steamvr::RepairNeeded(L, g_opt.installDir);
        if (w != steamvr::RepairWhy::None) {
            repair = true;
            if (r.repairWhy == steamvr::RepairWhy::None) r.repairWhy = w;
        }
        else if (L.state == LocState::Enabled) on = true;
        else if (L.state == LocState::OriginalMissing) orig = true;
        else if (L.state == LocState::OpenComposite) oc = true;
        else if (L.state == LocState::NotEnabled) off = true;
    }
    if (repair) return RowState::NeedsRepair;
    if (on) return RowState::On;
    if (orig) return RowState::OriginalMissing;
    if (oc) return RowState::OpenComposite;
    if (off) return RowState::Off;
    if (r.dirs.empty() && !r.dirs32.empty()) return RowState::Only32;
    return RowState::Unknown;
}

// A box can be ticked / unticked: always to untick an enabled game; to tick, the game needs something Enable can act on.
bool Toggleable(const Row& r) {
    if (r.enabled) return true;
    return r.state == RowState::Off || r.state == RowState::NeedsRepair || r.state == RowState::On || r.state == RowState::OpenComposite;
}

// Message box owned by the window; every box and its answer is logged (tests read both).
int Box(const std::wstring& text, UINT flags) {
    const int r = glass::MessageBox(g_dlg, text, kWindowTitle, flags | MB_SETFOREGROUND);
    const char* ans = r == IDYES ? "Yes" : r == IDNO ? "No" : r == IDOK ? "OK" : r == IDCANCEL ? "Cancel" : "?";
    LOGI("tray: steamvr: box: %s -> %s", N(text).c_str(), ans);
    return r;
}

void LogSteps(const steamvr::ActionResult& r, const char* what, const std::wstring& dir) {
    for (const std::wstring& s : r.steps) LOGI("tray: steamvr:   %s", N(s).c_str());
    if (r.ok) LOGI("tray: steamvr: %s %s: %s", what, N(dir).c_str(), N(r.message).c_str());
    else LOGW("tray: steamvr: %s %s FAILED (error %lu): %s", what, N(dir).c_str(), r.error, N(r.message).c_str());
}

// ---- HKCU\Software\OpenXR-EyeCapture\Home = the install folder (REG_SZ, no admin): the SteamVR proxy's FALLBACK when a game
// folder has no sidecar (src/openvr/openvr_hooks.h; the sidecar already carries home). Written only when the user enabled or
// repaired a SteamVR game, and only when it differs -- never at tray start. Deleted when the user disabled the last game
// (DeleteHomeKey). Tests: the key comes from XR_EYE_CAPTURE_TEST_HOME_KEY (an HKCU subkey); a test-mode tray without it
// touches nothing.
std::wstring HomeKeyPath(bool testMode) {
    wchar_t keyBuf[512];
    const DWORD n = GetEnvironmentVariableW(EYECAP_TEST_HOME_KEY_ENV, keyBuf, 512);
    if (n > 0 && n < 512) return keyBuf;
    return testMode ? std::wstring() : std::wstring(EYECAP_HOME_REG_KEY);
}

void EnsureHomeKey(const std::wstring& installDir, bool testMode) {
    const std::wstring key = HomeKeyPath(testMode);
    if (key.empty()) return;
    wchar_t cur[1024] = L"";
    DWORD bytes = sizeof(cur) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), EYECAP_HOME_REG_VALUE, RRF_RT_REG_SZ, nullptr, cur, &bytes) == ERROR_SUCCESS &&
        _wcsicmp(cur, installDir.c_str()) == 0)
        return;
    HKEY k = nullptr;
    LSTATUS e = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (e == ERROR_SUCCESS) {
        e = RegSetValueExW(k, EYECAP_HOME_REG_VALUE, 0, REG_SZ, (const BYTE*)installDir.c_str(), (DWORD)((installDir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    if (e == ERROR_SUCCESS) LOGI("tray: SteamVR home: HKCU\\%s\\Home = %s", N(key).c_str(), N(installDir).c_str());
    else LOGW("tray: SteamVR home: cannot write HKCU\\%s\\Home (error %ld)", N(key).c_str(), (long)e);
}

// The user disabled the last SteamVR game: the Home value goes (only when it names this install folder or a folder that no
// longer exists -- another install's value is left alone), and the key too when nothing else is in it.
void DeleteHomeKey(const std::wstring& installDir, bool testMode) {
    const std::wstring key = HomeKeyPath(testMode);
    if (key.empty()) return;
    wchar_t cur[1024] = L"";
    DWORD bytes = sizeof(cur) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), EYECAP_HOME_REG_VALUE, RRF_RT_REG_SZ, nullptr, cur, &bytes) != ERROR_SUCCESS) return;
    if (_wcsicmp(cur, installDir.c_str()) != 0 && DirExists(cur)) {
        LOGI("tray: SteamVR home: HKCU\\%s\\Home = %s (another install) -- left alone", N(key).c_str(), N(cur).c_str());
        return;
    }
    const LSTATUS e = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), EYECAP_HOME_REG_VALUE);
    if (e != ERROR_SUCCESS) { LOGW("tray: SteamVR home: cannot delete HKCU\\%s\\Home (error %ld)", N(key).c_str(), (long)e); return; }
    LOGI("tray: SteamVR home: no SteamVR game enabled any more -- deleted HKCU\\%s\\Home", N(key).c_str());
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD subKeys = 1, values = 1;
    const LSTATUS q = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    if (q == ERROR_SUCCESS && subKeys == 0 && values == 0 && RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS)
        LOGI("tray: SteamVR home: deleted the empty key HKCU\\%s", N(key).c_str());
}

std::vector<GameEntry> LoadGamesAt(const std::wstring& ini) {
    std::vector<GameEntry> g;
    if (!steamvr::LoadGameList(ini, g)) g.clear();
    return g;
}
std::vector<GameEntry> LoadGames() { return LoadGamesAt(IniPath()); }
// Read-modify-write of the [game] entries of <installDir>\steamvr_games.ini under the ini lock (the [found] cache is kept).
// A change that removes the last game deletes the Home value (DeleteHomeKey).
bool SaveGamesAt(const std::wstring& installDir, bool testMode, const std::function<void(std::vector<GameEntry>&)>& change) {
    const std::wstring ini = IniPathFor(installDir);
    bool ok = false, lastGone = false;
    {
        std::lock_guard<std::mutex> lk(g_iniM);
        std::vector<GameEntry> games = LoadGamesAt(ini);
        const bool had = !games.empty();
        change(games);
        ok = steamvr::SaveGameList(ini, games);
        lastGone = ok && had && games.empty();
    }
    if (!ok) LOGW("tray: steamvr: cannot write %s", N(ini).c_str());
    if (lastGone) DeleteHomeKey(installDir, testMode);
    return ok;
}
bool SaveGames(const std::function<void(std::vector<GameEntry>&)>& change) { return SaveGamesAt(g_opt.installDir, g_opt.testMode, change); }

// Disables every folder in `dirs` of one game (DisableLocation each); results += one line per folder. Returns the folders
// that still hold our files (refused / failed: they stay in the list); a missing folder or one with nothing of ours left is
// dropped.
std::vector<std::wstring> DisableGameFolders(const std::wstring& name, const std::wstring& root, const std::vector<std::wstring>& dirs,
                                             std::wstring& results) {
    std::vector<std::wstring> remaining;
    for (const std::wstring& d : dirs) {
        if (!DirExists(d)) {
            results += L"• " + Rel(root, d) + L"\n    Folder not found. Removed from the list.\n";
            LOGI("tray: steamvr: disable %s: %s not found, dropped from the list", N(name).c_str(), N(d).c_str());
            continue;
        }
        const Location L = steamvr::InspectLocation(d, std::string());
        const steamvr::ActionResult r = steamvr::DisableLocation(L);
        LogSteps(r, "disable", d);
        // refused with nothing of ours in that folder (32-bit / OpenComposite / unknown DLL now): dropped from the list too
        const bool nothingOurs = !r.ok && r.error == 0 && L.state != LocState::OriginalMissing;
        if (!r.ok && !nothingOurs) remaining.push_back(d);
        results += L"• " + Rel(root, d) + L"\n    " + Sentence(r.message) + L"\n";
    }
    return remaining;
}

int SelectedRow() {
    const HWND lb = GetDlgItem(g_dlg, ID_SVR_LIST);
    const int sel = lb ? (int)SendMessageW(lb, LB_GETCURSEL, 0, 0) : -1;
    return (sel < 0 || (size_t)sel >= g_rows.size()) ? -1 : sel;
}

void UpdateDetails() {
    const int ri = SelectedRow();
    std::wstring t;
    if (g_rows.empty()) {
        t = L"No SteamVR games found. Steam games appear here after the scan; for other games click \"Add a game by exe...\" and pick "
            L"the game's .exe.";
    } else if (ri < 0) {
        t = L"Select a game for its details. Tick a game to record it through SteamVR; untick it to put the game's own "
            L"openvr_api.dll back.";
    } else {
        const Row& r = g_rows[(size_t)ri];
        t = r.name + (r.appid.empty() ? std::wstring() : L" (Steam app " + r.appid + L")") + L"\r\nGame folder: " + r.root;
        if (!r.exe.empty()) t += L"\r\nExe: " + r.exe;
        t += r.enabled ? L"\r\nRecording enabled by you (after a game update the tray app tells you, and Repair puts it back)." : L"\r\nNot enabled.";
        if (r.state == RowState::NeedsRepair)
            t += L"\r\nNeeds repair: " + elog::Widen(steamvr::RepairWhyName(r.repairWhy)) + L". Click Repair (or Re-enable); close the game first.";
        if (r.antiCheat) t += L"\r\nAnti-cheat (EasyAntiCheat / BattlEye) found in the game folder.";
        if (r.truncated) t += L"\r\nThe game folder is very large: the Steam scan stopped early there (ticking the game searches it fully).";
        for (const Location& L : r.locs)
            for (const std::wstring& k : L.staleKept) t += L"\r\nLeftover " + k + L" is still in use (the game runs?): removed later.";
        for (const Location& L : r.locs) {
            t += L"\r\n" + L.dir + L"\r\n    " + LocText(L);
            if (DirExists(L.dir)) {
                t += L" -- openvr_api.dll: " + elog::Widen(steamvr::DllKindName(L.dll.kind));
                if (!L.dll.proxyVersion.empty()) t += L" " + elog::Widen(L.dll.proxyVersion);
                t += L", openvr_api_real.dll: " + elog::Widen(steamvr::DllKindName(L.real.kind));
            }
        }
        for (const std::wstring& d : r.dirs32) t += L"\r\n" + d + L"\r\n    32-bit -- not supported";
    }
    SetDlgItemTextW(g_dlg, ID_SVR_DETAILS, t.c_str());
}

void UpdateStatus() {
    std::wstring s;
    {
        std::lock_guard<std::mutex> lk(g_scan.m);
        s = g_scan.running ? std::wstring(L"Scanning the Steam library...") : g_scan.summary;
    }
    if (!g_status.empty()) s += (s.empty() ? L"" : L"   ") + g_status;
    SetDlgItemTextW(g_dlg, ID_SVR_STATUS, s.c_str());
}

void UpdateButtons() {
    const bool sel = SelectedRow() >= 0;
    EnableWindow(GetDlgItem(g_dlg, ID_SVR_DISABLE), sel && g_rows[(size_t)SelectedRow()].enabled);
    EnableWindow(GetDlgItem(g_dlg, ID_SVR_REENABLE), sel && g_rows[(size_t)SelectedRow()].enabled);
    bool anyOff = false, anyOn = false, anyRepair = false;
    for (const Row& r : g_rows) {
        anyOff = anyOff || (!r.enabled && r.state == RowState::Off);
        anyOn = anyOn || r.enabled;
        anyRepair = anyRepair || (r.enabled && r.state == RowState::NeedsRepair);
    }
    EnableWindow(GetDlgItem(g_dlg, ID_SVR_DISABLEALL), anyOn);
    EnableWindow(GetDlgItem(g_dlg, ID_SVR_REPAIR), anyRepair);
    bool running = false;
    { std::lock_guard<std::mutex> lk(g_scan.m); running = g_scan.running; }
    EnableWindow(GetDlgItem(g_dlg, ID_SVR_ENABLEALL), anyOff && !running);
    EnableWindow(GetDlgItem(g_dlg, ID_SVR_RESCAN), !running);
}

// Re-reads steamvr_games.ini ([game] + [found]) and every DLL folder; keeps the selection on the game with root `keepRoot`.
void Refresh(const std::wstring& selectRoot = std::wstring()) {
    std::wstring keep = selectRoot;
    if (keep.empty()) {
        const int ri = SelectedRow();
        if (ri >= 0) keep = g_rows[(size_t)ri].root;
    }
    const std::wstring src = FindProxySource(g_opt.installDir);
    g_ourVersion = src.empty() ? std::string(EYE_CAPTURE_VERSION) : steamvr::InspectDll(src).proxyVersion;
    std::vector<GameEntry> games;
    std::vector<FoundGame> found;
    {
        std::lock_guard<std::mutex> lk(g_iniM);
        games = LoadGames();
        if (!steamvr::LoadFoundGames(IniPath(), found)) found.clear();
    }
    g_rows.clear();
    auto rowFor = [&](const std::wstring& root) -> Row& {
        for (Row& r : g_rows) if (SameDir(r.root, root)) return r;
        g_rows.emplace_back();
        g_rows.back().root = root;
        return g_rows.back();
    };
    for (const GameEntry& g : games) {
        Row& r = rowFor(g.root);
        r.name = g.name;
        r.exe = g.exe;
        r.appid = g.appid;
        r.enabled = true;
        for (const std::wstring& d : g.dlls) AddUnique(r.dirs, d);
    }
    for (const FoundGame& f : found) {
        Row& r = rowFor(f.root);
        if (r.name.empty()) r.name = f.name;
        if (r.appid.empty()) r.appid = f.appid;
        r.steam = true;
        r.antiCheat = f.antiCheat;
        r.truncated = f.truncated;
        for (const std::wstring& d : f.dlls) AddUnique(r.dirs, d);
        for (const std::wstring& d : f.dlls32) AddUnique(r.dirs32, d);
    }
    for (Row& r : g_rows) {
        for (const std::wstring& d : r.dirs) {
            Location L = DirExists(d) ? steamvr::InspectLocation(d, g_ourVersion) : Location();
            L.dir = d;
            r.locs.push_back(L);
        }
        r.state = StateOf(r);
    }
    std::sort(g_rows.begin(), g_rows.end(), [](const Row& a, const Row& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    const HWND lb = GetDlgItem(g_dlg, ID_SVR_LIST);
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    int selIdx = -1;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        const std::wstring line = r.name + L"\t" + StateText(r.state, r.repairWhy) + L"\t" + r.root;
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        if (selIdx < 0 && !keep.empty() && SameDir(r.root, keep)) selIdx = (int)i;
    }
    SendMessageW(lb, LB_SETCURSEL, (WPARAM)selIdx, 0);
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, nullptr, TRUE);
    UpdateDetails();
    UpdateButtons();
    UpdateStatus();
}

// The first non-empty line of the file named by EYECAP_TRAY_TEST_PICK (tests), read at every pick.
std::wstring TestPick() {
    wchar_t pickFile[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_PICK", pickFile, MAX_PATH) == 0) return std::wstring();
    HANDLE h = CreateFileW(pickFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string text;
    if (h != INVALID_HANDLE_VALUE) {
        char b[4096];
        DWORD got = 0;
        while (ReadFile(h, b, sizeof(b), &got, nullptr) && got) text.append(b, got);
        CloseHandle(h);
    }
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) return elog::Widen(line);
    }
    return std::wstring();
}

std::wstring PickExe() {
    if (g_opt.testMode && GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_PICK", nullptr, 0) > 0) {
        const std::wstring t = TestPick();
        LOGI("tray: steamvr: EYECAP_TRAY_TEST_PICK -> %s, no dialog (test)", N(t).c_str());
        return t;
    }
    wchar_t file[MAX_PATH * 2] = L"";
    const std::wstring steam = steamvr::SteamInstallDir(g_opt.testMode);
    const std::wstring start = steam.empty() || !DirExists(steam + L"\\steamapps\\common") ? std::wstring() : steam + L"\\steamapps\\common";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_dlg;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
    ofn.lpstrTitle = L"Pick the game's .exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&ofn)) return std::wstring();
    return file;
}

// Enable (or re-enable) one game: scan, OpenComposite question, confirmation, enable every 64-bit folder, save the list.
void EnableFlow(const std::wstring& exe, const std::wstring& root, const std::wstring& name, const std::wstring& appid, bool reenable) {
    const char* what = reenable ? "re-enable" : "enable";
    std::wstring tried;
    const std::wstring src = FindProxySource(g_opt.installDir, &tried);
    if (src.empty()) {
        LOGE("tray: steamvr: %s %s: no proxy found (looked in %s)", what, N(name).c_str(), N(tried).c_str());
        Box(L"The VR EyeCapture recorder for SteamVR games (openvr_api.dll) was not found. Looked for:\n" + tried +
                L"\n\nReinstall VR EyeCapture: the zip has it as steamvr\\openvr_api.dll.",
            MB_OK | MB_ICONERROR);
        return;
    }
    const steamvr::DllInfo srcInfo = steamvr::InspectDll(src);
    const std::string ver = srcInfo.proxyVersion;
    if (!DirExists(root)) {
        LOGW("tray: steamvr: %s %s: game folder %s not found", what, N(name).c_str(), N(root).c_str());
        Box(name + L": the game folder was not found:\n" + root + L"\n\nIs the game still installed? \"Disable\" removes it from the list.",
            MB_OK | MB_ICONWARNING);
        return;
    }
    LOGI("tray: steamvr: %s %s: exe %s, scanning %s (proxy %s, version %s)", what, N(name).c_str(), N(exe).c_str(), N(root).c_str(),
         N(src).c_str(), ver.c_str());
    steamvr::ScanResult sr = steamvr::ScanGame(root, ver);
    for (const Location& L : sr.locations)
        LOGI("tray: steamvr:   found %s: %s (openvr_api.dll %s)", N(L.dir).c_str(), steamvr::LocStateName(L.state),
             steamvr::DllKindName(L.dll.kind));
    for (const std::wstring& a : sr.antiCheat) LOGI("tray: steamvr:   anti-cheat: %s", N(a).c_str());
    if (sr.locations.empty()) {
        Box(name + L": no openvr_api.dll found in\n" + root +
                L"\n\nThis game does not use SteamVR's OpenVR API from its own folder. If it is an OpenXR game, VR EyeCapture records "
                L"it without this step once it is registered for OpenXR games (tray menu -> Register for VR games...).",
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    for (Location& L : sr.locations) {
        if (L.state != LocState::OpenComposite || !L.hasSteamVrBackup) continue;
        const std::wstring q =
            L"OpenComposite is installed for " + name + L" in\n" + L.dir +
            L"\n\nOpenComposite and VR EyeCapture's SteamVR recording are alternatives for the same game. With OpenComposite the "
            L"game talks OpenXR (VR EyeCapture records that route once it is registered for OpenXR games); SteamVR recording "
            L"needs SteamVR's own openvr_api.dll.\n\nRestore SteamVR's openvr_api.dll here first? (OpenComposite's file is kept "
            L"as openvr_api.dll.opencomposite; OpenComposite's switcher can turn it on again.)";
        if (Box(q, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) continue;
        const steamvr::ActionResult r = steamvr::RestoreSteamVrDll(L);
        LogSteps(r, "restore SteamVR's openvr_api.dll in", L.dir);
        if (!r.ok) Box(L"Could not restore SteamVR's openvr_api.dll in\n" + L.dir + L"\n\n" + r.message, MB_OK | MB_ICONERROR);
        L = steamvr::InspectLocation(L.dir, ver);
    }
    std::vector<size_t> todo;
    std::wstring list;   // one bullet per openvr_api.dll folder: the folder, then what happens there on the next line
    for (size_t i = 0; i < sr.locations.size(); ++i) {
        const Location& L = sr.locations[i];
        std::wstring plan;
        switch (L.state) {
        case LocState::NotEnabled: plan = L"Will be enabled."; todo.push_back(i); break;
        case LocState::ProxyMissing: plan = L"Will be enabled again (the game was updated)."; todo.push_back(i); break;
        case LocState::Enabled:
            plan = steamvr::RepairNeeded(L, g_opt.installDir) == steamvr::RepairWhy::Moved
                       ? L"Already enabled. It will point to VR EyeCapture's new folder."
                   : L.proxyOutdated ? L"Already enabled. The recorder file will be updated." : L"Already enabled. It will be refreshed.";
            todo.push_back(i);
            break;
        case LocState::Unsupported32: plan = L"Skipped: 32-bit games are not supported."; break;
        case LocState::OpenComposite:
            plan = L.hasSteamVrBackup ? L"Skipped: OpenComposite is installed here (left as it is)."
                                      : L"Skipped: OpenComposite is installed here, and there is no SteamVR copy to put back.";
            break;
        case LocState::OriginalMissing: plan = L"Skipped: the game's original file is missing. In Steam, verify the game files."; break;
        default: plan = L"Skipped: this is not Valve's openvr_api.dll."; break;
        }
        if (!todo.empty() && todo.back() == i) {   // the same checks EnableLocation makes first: shown here, not acted on
            const steamvr::DllInfo* gd = steamvr::GameDllOf(L);
            const std::vector<std::string> miss = gd ? steamvr::MissingExports(*gd, srcInfo) : std::vector<std::string>();
            steamvr::ActionResult busy;
            if (!miss.empty()) { plan = Sentence(steamvr::MissingExportsText(miss)); todo.pop_back(); }
            else if (steamvr::RefuseInUse(L, busy)) { plan = Sentence(busy.message); todo.pop_back(); }
        }
        list += L"• " + Rel(root, L.dir) + L"\n    " + plan + L"\n";
    }
    if (sr.truncated) list += L"(The folder is very large: the search stopped early.)\n";
    std::wstring anti;
    if (!sr.antiCheat.empty()) {
        anti = L"\nWARNING: anti-cheat found (" + Rel(root, sr.antiCheat[0]) + (sr.antiCheat.size() > 1 ? L" and more" : L"") +
               L"). EasyAntiCheat or BattlEye may block the game or flag the changed file. In an online game, that can get your "
               L"account banned. Only enable it for offline games, at your own risk.\n";
    }
    if (todo.empty()) {
        Box(name + L" cannot be recorded through SteamVR.\n\nGame folder:\n" + root + L"\n\nopenvr_api.dll found in:\n" + list + anti,
            MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring q = std::wstring(reenable ? L"Re-enable" : L"Enable") + L" SteamVR recording for " + name +
                           L"?\n\nClose the game and SteamVR first.\n\nGame folder:\n" + root + L"\n\nopenvr_api.dll found in:\n" +
                           list + anti +
                           L"\nThe game's own file stays next to it as openvr_api_real.dll. Untick the game to put it back.";
    if (Box(q, MB_YESNO | (anti.empty() ? MB_ICONQUESTION : MB_ICONWARNING | MB_DEFBUTTON2)) != IDYES) {
        LOGI("tray: steamvr: %s %s: cancelled", what, N(name).c_str());
        return;
    }
    GameEntry e;
    e.name = name;
    e.exe = exe;
    e.root = root;
    e.appid = appid;
    std::wstring results;
    size_t okCount = 0;
    for (size_t i : todo) {
        const Location& L = sr.locations[i];
        const steamvr::ActionResult r = steamvr::EnableLocation(L, src, g_opt.installDir, name);
        LogSteps(r, what, L.dir);
        if (r.ok) { ++okCount; e.dlls.push_back(L.dir); }
        results += L"• " + Rel(root, L.dir) + L"\n    " + Sentence(r.message) + L"\n";
    }
    if (okCount) EnsureHomeKey(g_opt.installDir, g_opt.testMode);
    bool saved = true;
    SaveGames([&](std::vector<GameEntry>& games) {
        // folders listed before that this scan did not touch but that still hold our files stay in the list
        for (const GameEntry& old : games) {
            if (!SameDir(old.root, root)) continue;
            if (e.exe.empty()) e.exe = old.exe;
            if (e.appid.empty()) e.appid = old.appid;
            for (const std::wstring& d : old.dlls) {
                bool have = false;
                for (const std::wstring& x : e.dlls) have = have || SameDir(x, d);
                if (have || !DirExists(d)) continue;
                const Location x = steamvr::InspectLocation(d, ver);
                if (x.state == LocState::Enabled || x.state == LocState::ProxyMissing || x.state == LocState::OriginalMissing) e.dlls.push_back(d);
            }
        }
        if (!e.dlls.empty()) steamvr::UpsertGame(games, e);
    }) || (saved = false);
    const std::wstring saveErr = saved ? std::wstring() : L"\n\n(The list " + IniPath() + L" could not be written; the game folder is changed anyway.)";
    LOGI("tray: steamvr: %s %s: %zu of %zu folder(s) done", what, N(name).c_str(), okCount, todo.size());
    // success: no box (the row turns On); a box only when something did not work
    if (okCount == todo.size()) {
        if (!saved) Box(L"SteamVR recording is enabled for " + name + L"." + saveErr, MB_OK | MB_ICONWARNING);
    } else {
        Box(name + L": " + std::to_wstring(okCount) + L" of " + std::to_wstring(todo.size()) + L" folder(s) enabled.\n\n" + results +
                L"\nClose the game and SteamVR, then try again. A folder that needs administrator rights cannot be changed from here." +
                saveErr,
            MB_OK | MB_ICONWARNING);
    }
    Refresh(root);
}

void DisableRow(const Row& g) {
    std::wstring where;
    for (const std::wstring& d : g.dirs) where += L"• " + Rel(g.root, d) + L"\n";
    if (where.empty()) where = L"(no folder recorded)\n";
    if (Box(L"Disable SteamVR recording for " + g.name + L"?\n\nClose the game first.\n\nThe game's own openvr_api.dll is put back in:\n" +
                where,
            MB_YESNO | MB_ICONQUESTION) != IDYES) {
        LOGI("tray: steamvr: disable %s: cancelled", N(g.name).c_str());
        return;
    }
    std::wstring results;
    const std::vector<std::wstring> remaining = DisableGameFolders(g.name, g.root, g.dirs, results);
    SaveGames([&](std::vector<GameEntry>& games) {
        if (remaining.empty()) {
            steamvr::RemoveGame(games, g.root);
            return;
        }
        for (GameEntry& e : games)
            if (SameDir(e.root, g.root)) e.dlls = remaining;
    });
    LOGI("tray: steamvr: disable %s: %s", N(g.name).c_str(), remaining.empty() ? "done, removed from the list" : "some folders failed");
    if (!remaining.empty())   // success: no box (the row turns Off)
        Box(g.name + L": not every folder could be changed.\n\n" + results + L"\nClose the game and SteamVR, then try again.",
            MB_OK | MB_ICONWARNING);
    Refresh(g.root);
}

void OnAddByExe() {
    const std::wstring exe = PickExe();
    if (exe.empty()) return;
    EnableFlow(exe, steamvr::GameRootForExe(exe), steamvr::GameNameForExe(exe), std::wstring(), false);
}

void ToggleRow(int ri) {
    if (ri < 0 || (size_t)ri >= g_rows.size()) return;
    const Row r = g_rows[(size_t)ri];
    if (!Toggleable(r)) {
        LOGI("tray: steamvr: %s: %s -- cannot be ticked", N(r.name).c_str(), N(StateText(r.state, r.repairWhy)).c_str());
        MessageBeep(MB_ICONWARNING);
        return;
    }
    LOGI("tray: steamvr: check box %s %s", N(r.name).c_str(), r.enabled ? "unticked" : "ticked");
    if (r.enabled) DisableRow(r);
    else EnableFlow(r.exe, r.root, r.name, r.appid, false);
}

void OnEnableAll() {
    std::wstring tried;
    const std::wstring src = FindProxySource(g_opt.installDir, &tried);
    if (src.empty()) {
        Box(L"The VR EyeCapture recorder for SteamVR games (openvr_api.dll) was not found. Looked for:\n" + tried, MB_OK | MB_ICONERROR);
        return;
    }
    std::vector<size_t> todo;
    std::wstring list, skipped;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (r.enabled) continue;
        if (r.state == RowState::Off && r.antiCheat) skipped += L"• " + r.name + L": anti-cheat found. Tick it on its own if you want it.\n";
        else if (r.state == RowState::OpenComposite) skipped += L"• " + r.name + L": OpenComposite is installed. Tick it on its own.\n";
        else if (r.state == RowState::Only32) skipped += L"• " + r.name + L": 32-bit games are not supported.\n";
        else if (r.state == RowState::Off) { todo.push_back(i); list += L"• " + r.name + L"\n"; }
    }
    if (todo.empty()) {
        Box(L"Nothing to enable." + (skipped.empty() ? std::wstring() : L"\n\nSkipped:\n" + skipped), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (Box(L"Enable SteamVR recording for these games?\n\nClose the games and SteamVR first.\n\n" + list +
                (skipped.empty() ? std::wstring() : L"\nSkipped:\n" + skipped) +
                L"\nEach game's own file stays next to it as openvr_api_real.dll. Untick a game to put it back.",
            MB_YESNO | MB_ICONQUESTION) != IDYES) {
        LOGI("tray: steamvr: enable all: cancelled");
        return;
    }
    std::wstring results;
    size_t games = 0;
    std::vector<GameEntry> add;
    for (size_t i : todo) {
        const Row& r = g_rows[i];
        GameEntry e;
        e.name = r.name;
        e.root = r.root;
        e.exe = r.exe;
        e.appid = r.appid;
        std::wstring res;
        for (const Location& L0 : r.locs) {
            const Location L = steamvr::InspectLocation(L0.dir, g_ourVersion);
            if (L.state != LocState::NotEnabled) continue;
            const steamvr::ActionResult a = steamvr::EnableLocation(L, src, g_opt.installDir, r.name);
            LogSteps(a, "enable all:", L.dir);
            if (a.ok) e.dlls.push_back(L.dir);
            else res += L"\n    " + Rel(r.root, L.dir) + L": " + Sentence(a.message);
        }
        if (!e.dlls.empty()) { add.push_back(e); ++games; }
        results += L"• " + r.name + L": " + (e.dlls.empty() ? L"not enabled" : L"enabled") + res + L"\n";
    }
    if (games) EnsureHomeKey(g_opt.installDir, g_opt.testMode);
    SaveGames([&](std::vector<GameEntry>& g) { for (const GameEntry& e : add) steamvr::UpsertGame(g, e); });
    LOGI("tray: steamvr: enable all: %zu of %zu game(s) enabled", games, todo.size());
    if (games != todo.size())   // all enabled: no box (the rows turn On)
        Box(L"SteamVR recording enabled for " + std::to_wstring(games) + L" of " + std::to_wstring(todo.size()) + L" game(s).\n\n" +
                results + L"\nClose the games and SteamVR, then try again.",
            MB_OK | MB_ICONWARNING);
    Refresh();
}

void OnReenable() {
    const int ri = SelectedRow();
    if (ri < 0) return;
    const Row r = g_rows[(size_t)ri];
    EnableFlow(r.exe, r.root, r.name, r.appid, true);
}

// "Repair": every folder of the enabled games that FindRepairs reports (game updated / older recorder / VR EyeCapture moved)
// is enabled again (RepairEnabledGames: in-use folders skipped, the export check applies). The user's click is the consent:
// nothing is repaired without it. One result box.
void OnRepair() {
    std::wstring tried;
    const std::wstring src = FindProxySource(g_opt.installDir, &tried);
    if (src.empty()) {
        Box(L"The VR EyeCapture recorder for SteamVR games (openvr_api.dll) was not found. Looked for:\n" + tried, MB_OK | MB_ICONERROR);
        return;
    }
    const std::string ver = steamvr::InspectDll(src).proxyVersion;
    const std::vector<GameEntry> games = LoadGames();
    const std::vector<steamvr::RepairItem> items = steamvr::RepairEnabledGames(games, src, g_opt.installDir, ver);
    std::wstring done, skipped, failed;
    size_t ok = 0;
    for (const steamvr::RepairItem& it : items) {
        using W = steamvr::RepairItem::What;
        const char* w = it.what == W::Repaired ? "repaired after a game update" : it.what == W::Updated ? "recorder updated"
                      : it.what == W::Moved ? "settings point to this folder again" : it.what == W::SkippedInUse ? "skipped" : "FAILED";
        LOGI("tray: steamvr: repair (Repair clicked): %s, %s: %s -- %s", N(it.game).c_str(), N(it.dir).c_str(), w, N(it.message).c_str());
        const std::wstring line = L"  " + it.game + L" -- " + it.dir + L"\n      " + it.message + L"\n";
        if (it.what == W::SkippedInUse) skipped += line;
        else if (it.what == W::Failed) failed += line;
        else { done += line; ++ok; }
    }
    LOGI("tray: steamvr: repair (Repair clicked): %zu folder(s) repaired, %zu not", ok, items.size() - ok);
    if (ok) EnsureHomeKey(g_opt.installDir, g_opt.testMode);
    g_status.clear();
    std::wstring t = items.empty() ? std::wstring(L"Nothing needs a repair.") : L"SteamVR recording repaired in " + std::to_wstring(ok) +
                                                                                  L" of " + std::to_wstring(items.size()) + L" folder(s).";
    if (!done.empty()) t += L"\n\nRepaired:\n" + done;
    if (!skipped.empty()) t += L"\n\nIn use (the game is running) -- close it and click Repair again:\n" + skipped;
    if (!failed.empty()) t += L"\n\nNot repaired:\n" + failed;
    Box(t, MB_OK | (ok == items.size() ? MB_ICONINFORMATION : MB_ICONWARNING));
    Refresh();
}

struct DisableAllOutcome {
    size_t games = 0, done = 0;
    std::wstring results;   // one block per game
};
// Disables every [game] entry of <installDir>\steamvr_games.ini (DisableGameFolders), keeps the folders that failed in the
// list, and deletes the Home value when no game is left (SaveGamesAt). No questions: the callers ask first.
DisableAllOutcome DisableAllCore(const std::wstring& installDir, bool testMode, const char* why) {
    DisableAllOutcome o;
    std::vector<GameEntry> games;
    {
        std::lock_guard<std::mutex> lk(g_iniM);
        games = LoadGamesAt(IniPathFor(installDir));
    }
    o.games = games.size();
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> left;   // root -> folders that still hold our files
    for (const GameEntry& g : games) {
        std::wstring res;
        const std::vector<std::wstring> rem = DisableGameFolders(g.name, g.root, g.dlls, res);
        if (rem.empty()) ++o.done;
        left.emplace_back(g.root, rem);
        o.results += g.name + (rem.empty() ? L": the game's own openvr_api.dll is back\n" : L": not every folder could be changed\n") + res;
    }
    SaveGamesAt(installDir, testMode, [&](std::vector<GameEntry>& cur) {
        for (const auto& l : left) {
            if (l.second.empty()) { steamvr::RemoveGame(cur, l.first); continue; }
            for (GameEntry& e : cur)
                if (SameDir(e.root, l.first)) e.dlls = l.second;
        }
    });
    LOGI("tray: steamvr: disable all (%s): %zu of %zu game(s) disabled", why, o.done, o.games);
    return o;
}

// "Disable all": one confirmation listing the enabled games, then DisableAllCore.
void OnDisableAll() {
    const std::vector<GameEntry> games = LoadGames();
    if (games.empty()) {
        Box(L"No SteamVR game is enabled.", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring list;
    for (const std::wstring& n : EnabledGameNames(g_opt.installDir)) list += L"• " + n + L"\n";   // sorted by name
    if (Box(L"Disable SteamVR recording for all " + std::to_wstring(games.size()) + L" game(s)?\n\nClose the games and SteamVR first.\n\n" +
                list + L"\nEach game's own openvr_api.dll is put back.",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        LOGI("tray: steamvr: disable all: cancelled");
        return;
    }
    const DisableAllOutcome o = DisableAllCore(g_opt.installDir, g_opt.testMode, "Disable all clicked");
    if (o.done != o.games)   // all disabled: no box (the rows turn Off)
        Box(L"SteamVR recording disabled for " + std::to_wstring(o.done) + L" of " + std::to_wstring(o.games) + L" game(s).\n\n" +
                o.results + L"\nClose the game and SteamVR, then try again.",
            MB_OK | MB_ICONWARNING);
    Refresh();
}

void OnDisable() {
    const int ri = SelectedRow();
    if (ri < 0) return;
    const Row r = g_rows[(size_t)ri];
    DisableRow(r);
}

// The Steam library scan on a worker thread; the result goes to the [found] cache, then WM_SVR_SCANDONE.
void StartScan(const char* why) {
    {
        std::lock_guard<std::mutex> lk(g_scan.m);
        if (g_scan.running) return;
        g_scan.running = true;
        g_scan.cancel = false;
        g_scan.notify = g_dlg;
    }
    UpdateStatus();
    UpdateButtons();
    const bool test = g_opt.testMode;
    const std::wstring ini = IniPath();
    const std::string ver = g_ourVersion;
    LOGI("tray: steamvr: Steam library scan started (%s)", why);
    std::thread([test, ini, ver] {
        const ULONGLONG t0 = GetTickCount64();
        const std::wstring steam = steamvr::SteamInstallDir(test);
        const std::vector<std::wstring> libs = steamvr::SteamLibraries(steam);
        bool budgetHit = false;
        std::vector<FoundGame> found = steamvr::DiscoverSteamGames(libs, ver, &g_scan.cancel, &budgetHit);
        std::wstring summary;
        if (steam.empty()) summary = L"Steam not found -- add games with \"Add a game by exe...\".";
        else summary = L"Steam library: " + std::to_wstring(found.size()) + L" SteamVR game(s) in " + std::to_wstring(libs.size()) +
                       L" librar" + (libs.size() == 1 ? L"y" : L"ies") + L".";
        size_t partly = 0;
        for (const FoundGame& fg : found) partly += fg.truncated ? 1 : 0;
        if (partly) summary += L" " + std::to_wstring(partly) + L" game folder(s) only partly searched.";
        if (budgetHit) summary += L" The scan stopped after 90 s: some games may be missing (Add a game by exe...).";
        bool cancelled = g_scan.cancel;
        if (!cancelled) {
            std::lock_guard<std::mutex> lk(g_iniM);
            std::vector<GameEntry> games;
            if (!steamvr::LoadGameList(ini, games)) games.clear();
            if (!steamvr::SaveGameList(ini, games, &found)) LOGW("tray: steamvr: cannot write the scan cache to %s", N(ini).c_str());
        }
        LOGI("tray: steamvr: Steam library scan %s: %s (Steam %s, %zu libraries, %llu ms)", cancelled ? "cancelled" : "done",
             N(summary).c_str(), steam.empty() ? "not found" : N(steam).c_str(), libs.size(), (unsigned long long)(GetTickCount64() - t0));
        for (const FoundGame& f : found)
            LOGI("tray: steamvr:   found %s (app %s): %zu 64-bit, %zu 32-bit openvr_api.dll folder(s)%s", N(f.name).c_str(),
                 N(f.appid).c_str(), f.dlls.size(), f.dlls32.size(), f.antiCheat ? ", anti-cheat" : "");
        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_scan.m);
            g_scan.running = false;
            g_scan.summary = summary;
            notify = g_scan.notify;
        }
        if (notify) PostMessageW(notify, WM_SVR_SCANDONE, 0, 0);
    }).detach();
}

// ---- steamvr_mode (config.ini [steamvr]): the "Recording mode" drop-down writes it in place
const wchar_t* const kModeKeys[3] = {L"per_game", L"quick", L"off"};
const wchar_t* const kModeItems[3] = {
    L"Per game (best quality, needs Enable per game)",
    L"Quick mode (no setup; lower quality, SteamVR menus visible, flat only -- no VR180/dome)",
    L"Off (SteamVR games are not recorded)"};

// Sets `key = value` in [section] of the ini at `path`: the existing line is rewritten in place (its comment lines, the
// other lines, the line endings and a BOM stay), a missing key is added after the section header (a missing section at the
// end). false = unreadable / unwritable. *old (optional) = the previous value ("" = none).
bool SetIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value, std::wstring* old) {
    std::string bytes;
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char b[65536];
            DWORD got = 0;
            while (ReadFile(h, b, sizeof(b), &got, nullptr) && got) bytes.append(b, got);
            CloseHandle(h);
        }
    }
    const bool bom = bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF;
    if (bom) bytes.erase(0, 3);
    const std::string nl = bytes.find("\r\n") != std::string::npos || bytes.empty() ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (size_t p = 0; p < bytes.size();) {
        size_t e = bytes.find('\n', p);
        if (e == std::string::npos) e = bytes.size();
        std::string l = bytes.substr(p, e - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        p = e + 1;
    }
    const std::string sec = "[" + N(section) + "]", k = N(key), v = N(value);
    auto trim = [](std::string s) { size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t"); return a == std::string::npos ? std::string() : s.substr(a, b - a + 1); };
    auto lower = [](std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
    int secAt = -1, keyAt = -1;
    std::string cur;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        if (!t.empty() && t[0] == '[') { cur = lower(t); if (cur == lower(sec) && secAt < 0) secAt = (int)i; continue; }
        if (cur != lower(sec) || t.empty() || t[0] == ';' || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(k)) keyAt = (int)i;   // the last one wins (as the parser)
    }
    if (old) old->clear();
    if (keyAt >= 0) {
        const size_t eq = lines[(size_t)keyAt].find('=');
        if (old) *old = elog::Widen(trim(lines[(size_t)keyAt].substr(eq + 1)));
        lines[(size_t)keyAt] = lines[(size_t)keyAt].substr(0, eq + 1) + " " + v;
    } else if (secAt >= 0) {
        lines.insert(lines.begin() + secAt + 1, k + " = " + v);
    } else {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back(sec);
        lines.push_back(k + " = " + v);
    }
    std::string out = bom ? "\xEF\xBB\xBF" : "";
    for (size_t i = 0; i < lines.size(); ++i) out += lines[i] + (i + 1 < lines.size() || !bytes.empty() ? nl : "");
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr) && w == out.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void LoadMode() {
    Config c;
    std::vector<std::wstring> problems;
    if (!g_opt.configPath.empty()) ParseConfigFile(g_opt.configPath, c, problems);
    const int sel = c.steamvr.mode == SteamVrMode::Quick ? 1 : c.steamvr.mode == SteamVrMode::Off ? 2 : 0;
    SendMessageW(GetDlgItem(g_dlg, ID_SVR_MODE), CB_SETCURSEL, (WPARAM)sel, 0);
}

void OnModeChanged() {
    const int sel = (int)SendMessageW(GetDlgItem(g_dlg, ID_SVR_MODE), CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 2 || g_opt.configPath.empty()) return;
    std::wstring old;
    if (!SetIniValue(g_opt.configPath, L"steamvr", L"steamvr_mode", kModeKeys[sel], &old)) {
        LOGW("tray: steamvr: cannot write steamvr_mode to %s", N(g_opt.configPath).c_str());
        Box(L"The recording mode could not be saved to\n" + g_opt.configPath, MB_OK | MB_ICONWARNING);
        LoadMode();
        return;
    }
    LOGI("tray: steamvr: steamvr_mode %s -> %s saved to %s", old.empty() ? "(none)" : N(old).c_str(), N(kModeKeys[sel]).c_str(),
         N(g_opt.configPath).c_str());
    g_status = sel == 1 ? L"Quick mode: SteamVR's mirror picture is recorded while SteamVR runs (games ticked here still record per game)."
             : sel == 2 ? L"Off: SteamVR games are not recorded." : L"Per game: only the ticked games are recorded.";
    UpdateStatus();
}

// Classic class names map to the glass controls (tray/glass.h); EDIT and LISTBOX stay native with the glass field look.
HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    const wchar_t* gc = !_wcsicmp(cls, L"BUTTON") ? glass::kButton : !_wcsicmp(cls, L"STATIC") ? glass::kStatic
                        : !_wcsicmp(cls, L"COMBOBOX") ? glass::kCombo : cls;
    HWND c = CreateWindowExW(ex, gc, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_dlg, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (!_wcsicmp(cls, L"EDIT") || !_wcsicmp(cls, L"LISTBOX")) glass::FrameEdit(c);
    return c;
}

// The list: a click in the check box column (or Space) toggles the row.
LRESULT CALLBACK ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_LBUTTONDOWN && (short)LOWORD(lp) < kCheckW) {
        const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, lp);
        const int idx = (int)LOWORD(hit);
        if (!HIWORD(hit) && idx >= 0 && (size_t)idx < g_rows.size()) {
            SendMessageW(h, LB_SETCURSEL, (WPARAM)idx, 0);
            UpdateDetails();
            UpdateButtons();
            PostMessageW(g_dlg, WM_COMMAND, MAKEWPARAM(ID_SVR_TOGGLE, 0), 0);
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && wp == VK_SPACE) {
        PostMessageW(g_dlg, WM_COMMAND, MAKEWPARAM(ID_SVR_TOGGLE, 0), 0);
        return 0;
    }
    if (msg == WM_GETDLGCODE) return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTCHARS;
    return DefSubclassProc(h, msg, wp, lp);
}

void DrawRow(const DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1) return;
    const bool sel = (di->itemState & ODS_SELECTED) != 0;
    COLORREF bgc, fgc;
    glass::ListRowColors(sel, GetFocus() == di->hwndItem, &bgc, &fgc);
    HBRUSH bgb = CreateSolidBrush(bgc);
    FillRect(di->hDC, &di->rcItem, bgb);
    DeleteObject(bgb);
    if (di->itemID >= g_rows.size()) return;
    const Row& r = g_rows[di->itemID];
    const int h = di->rcItem.bottom - di->rcItem.top, box = h - 6 > 16 ? 16 : h - 6;
    RECT cb{di->rcItem.left + 3, di->rcItem.top + (h - box) / 2, di->rcItem.left + 3 + box, di->rcItem.top + (h - box) / 2 + box};
    glass::DrawCheckBox(di->hDC, cb, r.enabled, Toggleable(r));
    SetBkMode(di->hDC, TRANSPARENT);
    COLORREF fg = fgc;
    if (r.state == RowState::NeedsRepair) fg = glass::Current().dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
    else if (r.state == RowState::Only32 || r.state == RowState::NotFound || r.state == RowState::Unknown) fg = glass::Current().textDim;
    SetTextColor(di->hDC, fg);
    const std::wstring cols[3] = {r.name, StateText(r.state, r.repairWhy), r.root};
    const int xs[3] = {kCheckW, kColState, kColFolder}, xe[3] = {kColState - 6, kColFolder - 6, di->rcItem.right - di->rcItem.left - 4};
    for (int i = 0; i < 3; ++i) {
        RECT tr{di->rcItem.left + xs[i], di->rcItem.top, di->rcItem.left + xe[i], di->rcItem.bottom};
        DrawTextW(di->hDC, cols[i].c_str(), (int)cols[i].size(), &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (i == 2 ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
    }
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) DrawFocusRect(di->hDC, &di->rcItem);
}

void BuildControls() {
    const int w = kClientW - 2 * kMargin;
    Make(L"STATIC",
         L"Records SteamVR (OpenVR) games. Tick a game: VR EyeCapture's openvr_api.dll goes into that game's folder and the game's "
         L"own one is kept as openvr_api_real.dll next to it; untick it to put the original back. Only that game changes. After a "
         L"game update or Steam's \"Verify integrity of game files\" the game shows \"Needs repair\": click Repair. OpenXR games do "
         L"not need this (Register for VR games).",
         SS_LEFT | SS_NOPREFIX, kMargin, 10, w, 64, ID_SVR_INTRO);
    Make(L"STATIC", L"Recording mode:", SS_LEFT | SS_NOPREFIX, kMargin, 82, 110, 18, -1);
    HWND mode = Make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kMargin + 112, 78, w - 112, 26, ID_SVR_MODE);
    for (const wchar_t* it : kModeItems) SendMessageW(mode, CB_ADDSTRING, 0, (LPARAM)it);
    Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kMargin, 110, w, 18, ID_SVR_STATUS);
    const int hy = 132;
    Make(L"STATIC", L"Game", SS_LEFT | SS_NOPREFIX, kMargin + 8 + kCheckW, hy, kColState - kCheckW - 4, 18, ID_SVR_HEADER);
    Make(L"STATIC", L"State", SS_LEFT | SS_NOPREFIX, kMargin + 8 + kColState, hy, kColFolder - kColState - 4, 18, -1);
    Make(L"STATIC", L"Folder", SS_LEFT | SS_NOPREFIX, kMargin + 8 + kColFolder, hy, w - kColFolder - 4, 18, -1);
    HWND lb = Make(L"LISTBOX", L"", WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                   kMargin, hy + 20, w, 240, ID_SVR_LIST, WS_EX_CLIENTEDGE);
    SetWindowSubclass(lb, ListProc, 1, 0);
    Make(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, kMargin, hy + 268, w, 100, ID_SVR_DETAILS,
         WS_EX_CLIENTEDGE);
    const int by = kClientH - kMargin - 28;
    Make(L"BUTTON", L"Enable all", WS_TABSTOP | BS_PUSHBUTTON, kMargin, by, 90, 28, ID_SVR_ENABLEALL);
    Make(L"BUTTON", L"Disable all", WS_TABSTOP | BS_PUSHBUTTON, kMargin + 96, by, 90, 28, ID_SVR_DISABLEALL);
    Make(L"BUTTON", L"Add a game by exe...", WS_TABSTOP | BS_PUSHBUTTON, kMargin + 192, by, 150, 28, ID_SVR_ENABLE);
    Make(L"BUTTON", L"Disable", WS_TABSTOP | BS_PUSHBUTTON, kMargin + 348, by, 80, 28, ID_SVR_DISABLE);
    Make(L"BUTTON", L"Re-enable", WS_TABSTOP | BS_PUSHBUTTON, kMargin + 434, by, 80, 28, ID_SVR_REENABLE);
    Make(L"BUTTON", L"Repair", WS_TABSTOP | BS_PUSHBUTTON, kMargin + 520, by, 80, 28, ID_SVR_REPAIR);
    Make(L"BUTTON", L"Rescan", WS_TABSTOP | BS_PUSHBUTTON, kMargin + 606, by, 80, 28, ID_SVR_RESCAN);
    Make(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, kClientW - kMargin - 90, by, 90, 28, ID_SVR_CLOSE);
}

// Colours that are not the theme's defaults (the column headers in grey); after BuildControls and on a theme change.
void ApplyGlassTheme() {
    for (HWND c = GetWindow(g_dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        const int id = GetDlgCtrlID(c);
        wchar_t t[16] = L"";
        GetWindowTextW(c, t, 16);
        if (id == ID_SVR_HEADER || (id == -1 && (!wcscmp(t, L"State") || !wcscmp(t, L"Folder")))) glass::SetDim(c, true);
    }
    InvalidateRect(GetDlgItem(g_dlg, ID_SVR_LIST), nullptr, TRUE);
}

LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SVR_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) { UpdateDetails(); UpdateButtons(); }
            return 0;
        case ID_SVR_TOGGLE: ToggleRow(SelectedRow()); return 0;
        case ID_SVR_MODE:
            if (HIWORD(wp) == CBN_SELCHANGE) OnModeChanged();
            return 0;
        case ID_SVR_ENABLE: OnAddByExe(); return 0;
        case ID_SVR_ENABLEALL: OnEnableAll(); return 0;
        case ID_SVR_DISABLE: OnDisable(); return 0;
        case ID_SVR_REENABLE: OnReenable(); return 0;
        case ID_SVR_REPAIR: OnRepair(); return 0;
        case ID_SVR_DISABLEALL: OnDisableAll(); return 0;
        case ID_SVR_RESCAN: StartScan("Rescan"); return 0;
        case ID_SVR_CLOSE:
        case IDCANCEL:   // Escape (IsDialogMessage)
            g_done = true;
            return 0;
        default: break;
        }
        break;
    case WM_SVR_SCANDONE:
        Refresh();
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlID == (UINT)ID_SVR_LIST) { mi->itemHeight = 22; return TRUE; }
        break;
    }
    case WM_DRAWITEM:
        if (wp == (WPARAM)ID_SVR_LIST) { DrawRow((const DRAWITEMSTRUCT*)lp); return TRUE; }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:   // the read-only details edit
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)glass::EditBrush((HDC)wp, (HWND)lp);
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        glass::PaintBackground(h, (HDC)wp, rc);
        return 1;
    }
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
        if (glass::OnSettingChange(h)) ApplyGlassTheme();
        return 0;
    case WM_CLOSE:
        g_done = true;
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

HWND DialogWindow() { return g_dlg; }

void RemoveHomeKey(const std::wstring& installDir, bool testMode) { DeleteHomeKey(installDir, testMode); }

std::wstring FindProxySource(const std::wstring& installDir, std::wstring* tried) {
    if (tried) tried->clear();
    for (const wchar_t* rel : {L"\\openvr_api.dll", L"\\steamvr\\openvr_api.dll", L"\\build\\Release\\openvr_api.dll"}) {
        const std::wstring p = installDir + rel;
        if (tried) *tried += L"  " + p + L"\n";
        if (!Exists(p)) continue;
        const steamvr::DllInfo d = steamvr::InspectDll(p);
        if (d.kind == steamvr::DllKind::Proxy && d.machine == steamvr::Machine::X64) return p;
    }
    return std::wstring();
}

RepairCheck CheckRepairs(const std::wstring& installDir, const char* why) {
    RepairCheck rc;
    std::vector<GameEntry> games;
    {
        std::lock_guard<std::mutex> lk(g_iniM);
        if (!steamvr::LoadGameList(IniPathFor(installDir), games) || games.empty()) return rc;
    }
    const std::wstring src = FindProxySource(installDir);
    if (src.empty()) {
        LOGW("tray: steamvr: repair check (%s): no proxy openvr_api.dll next to the tray -- nothing checked", why);
        return rc;
    }
    const std::string ver = steamvr::InspectDll(src).proxyVersion;
    std::vector<std::wstring> names;
    for (const steamvr::RepairNeed& n : steamvr::FindRepairs(games, installDir, ver)) {
        LOGI("tray: steamvr: repair check (%s): %s, %s: needs repair (%s)", why, N(n.game).c_str(), N(n.dir).c_str(),
             steamvr::RepairWhyName(n.why));
        AddUnique(names, n.game);
        ++rc.folders;
    }
    rc.games = names.size();
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (size_t i = 0; i < names.size(); ++i) rc.names += (i ? (i + 1 == names.size() ? L" and " : L", ") : L"") + names[i];
    LOGI("tray: steamvr: repair check (%s): %zu enabled game(s) checked, %zu need a repair -- nothing changed (Repair in the SteamVR "
         "games window does it)", why, games.size(), rc.games);
    return rc;
}

std::vector<std::wstring> EnabledGameNames(const std::wstring& installDir) {
    std::vector<std::wstring> out;
    std::lock_guard<std::mutex> lk(g_iniM);
    for (const GameEntry& g : LoadGamesAt(IniPathFor(installDir))) out.push_back(g.name);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

std::wstring DisableAllGames(const std::wstring& installDir, bool testMode, const char* why, bool* allOk) {
    const DisableAllOutcome o = DisableAllCore(installDir, testMode, why);
    if (allOk) *allOk = o.done == o.games;
    if (g_dlg) PostMessageW(g_dlg, WM_SVR_SCANDONE, 0, 0);   // the open window re-reads the list
    std::wstring t = L"SteamVR recording disabled for " + std::to_wstring(o.done) + L" of " + std::to_wstring(o.games) + L" game(s).";
    if (o.done != o.games) t += L"\n\n" + o.results + L"\nClose the game and SteamVR, then use SteamVR games... -> Disable all.";
    return t;
}

void ShowDialog(const Options& o) {
    if (g_dlg) {
        ShowWindow(g_dlg, SW_RESTORE);
        SetForegroundWindow(g_dlg);
        return;
    }
    g_opt = o;
    g_done = false;
    {   // read-only: the rows show "Needs repair", the Repair button acts
        const RepairCheck rc = CheckRepairs(o.installDir, "window opened");
        g_status = rc.games ? std::to_wstring(rc.games) + L" game(s) need a repair (" + rc.names + L"): click Repair." : std::wstring();
    }
    glass::Init();
    glass::Reload();   // the theme may have changed while no window was open
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   // the app icon (tray/tray.rc)
        wc.hbrBackground = nullptr;   // WM_ERASEBKGND: glass::PaintBackground
        wc.lpszClassName = kWindowClass;
        RegisterClassW(&wc);
        registered = true;
    }
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, kClientW, kClientH};
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_APPWINDOW);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) wa = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    const int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    g_dlg = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle, style, x < wa.left ? wa.left : x, y < wa.top ? wa.top : y,
                            w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_dlg) {
        LOGI("tray: steamvr: window opened (list %s)", N(IniPath()).c_str());
        glass::ApplyWindow(g_dlg);
        BuildControls();
        ApplyGlassTheme();
        LoadMode();
        Refresh();
        StartScan("window opened");
        ShowWindow(g_dlg, SW_SHOW);
        SetForegroundWindow(g_dlg);
        MSG m;
        while (!g_done) {
            const BOOL got = GetMessageW(&m, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage((int)m.wParam);   // the tray is exiting: pass WM_QUIT on
                break;
            }
            if (g_dlg && IsDialogMessageW(g_dlg, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        {
            std::lock_guard<std::mutex> lk(g_scan.m);
            g_scan.notify = nullptr;   // a scan still running finishes into the cache only
        }
        DestroyWindow(g_dlg);
        LOGI("tray: steamvr: window closed");
    }
    g_dlg = nullptr;
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
}

} // namespace steamvr_ui
