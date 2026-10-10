// EyeCaptureTray -- tray control for the OpenXR-EyeCapture layer (Win32, no frameworks). The layer is dormant (pure
// pass-through) unless this app is running: it holds the named mutex Local\OpenXR-EyeCapture-TrayAlive while it runs.
//
// Tray icon: grey = no game, green = game running (ready), red = recording, amber = starting / finalizing / failed,
// blue = converting a finished recording (post-processing, see tray/postprocess.h).
// Right-click (or left-click) menu: Start / Stop recording, Open captures folder, Convert last recording now, Open log,
// Edit config, Exit.
// Tooltip: game, API, resolution, fps, frames dropped, elapsed, conversion progress. Talks to the layer through
// src/shared_status.h. After each recording the tray remuxes it to .mkv (YouTube 3D tag) in a worker thread.
// Start it BEFORE the game: the layer decides at xrCreateSession whether to arm (config require_tray=0 = always arm).
#include <windows.h>
#include <shellapi.h>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include "config.h"
#include "postprocess.h"
#include "shared_status.h"

namespace {
const UINT WM_TRAY = WM_APP + 1;
const UINT WM_PP = WM_APP + 2;     // the post-processing worker changed state / queued a notice
const UINT kTimer = 1;
enum : UINT { ID_TOGGLE = 100, ID_FOLDER, ID_CONVERT, ID_LOG, ID_CONFIG, ID_EXIT };

HINSTANCE      g_inst;
HWND           g_wnd;
NOTIFYICONDATAW g_nid{};
EyeCapShared*  g_shm = nullptr;
HANDLE         g_map = nullptr;
HICON          g_icons[5] = {};   // 0 grey, 1 green, 2 red, 3 amber, 4 blue (converting)
int            g_iconIdx = -1;
EyeCapShared   g_st{};
bool           g_live = false;    // a live owner exists
uint32_t       g_prevState = ECS_NONE;
std::wstring   g_exeDir;
pp::Processor  g_pp;
bool           g_baselineDone = false;   // the first status read only sets the baseline (no job for older recordings)
std::wstring   g_lastSaved;              // last "saved" file seen (dedupe)
// Fallback scan of the captures folder (see ScanCaptures): covers a "saved" message that never arrived (game quit /
// crashed / was killed right after the recording).
FILETIME       g_trayStart{};                      // only MP4s newer than this are considered
std::set<std::wstring> g_queuedPaths;              // normalised full paths queued in this tray session (both paths dedupe here)
struct Pending { ULONGLONG size = 0; ULONGLONG stableSince = 0; std::wstring path; };
std::map<std::wstring, Pending> g_pending;         // scan candidates waiting for their size to settle
bool           g_wasLive = false;
ULONGLONG      g_lastScan = 0;

HICON MakeDotIcon(COLORREF c) {
    const int s = 32;
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = s;
    bh.bV5Height = -s;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    auto* px = (uint32_t*)bits;
    const double cx = (s - 1) / 2.0, r = s / 2.0 - 2;
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            const double d = sqrt((x - cx) * (x - cx) + (y - cx) * (y - cx));
            double a = r + 0.5 - d;                         // anti-aliased edge
            a = a < 0 ? 0 : (a > 1 ? 1 : a);
            const bool ring = d > r - 2.5;                  // dark rim
            const uint32_t rr = ring ? GetRValue(c) / 2 : GetRValue(c);
            const uint32_t gg = ring ? GetGValue(c) / 2 : GetGValue(c);
            const uint32_t bb = ring ? GetBValue(c) / 2 : GetBValue(c);
            const uint32_t al = (uint32_t)(a * 255);
            px[y * s + x] = (al << 24) | ((rr * al / 255) << 16) | ((gg * al / 255) << 8) | (bb * al / 255);
        }
    }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::wstring NormPath(const std::wstring& p) {
    wchar_t full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    std::wstring r = (n > 0 && n < sizeof(full) / sizeof(full[0])) ? std::wstring(full, n) : p;
    CharLowerBuffW(&r[0], (DWORD)r.size());
    return r;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Queue a finished recording for post-processing (MP4 only; a no-op when both post_mkv and post_flat are 0).
// Non-forced requests are deduplicated by full path for the whole tray session (status message vs fallback scan).
void QueueRecording(const std::wstring& mp4, bool force) {
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    if (!force && !cfg.postMkv && !cfg.postFlat) {
        LOGI("tray: recording %s saved; post_mkv and post_flat are both 0 -- not converting", elog::Narrow(mp4).c_str());
        return;
    }
    const std::wstring key = NormPath(mp4);
    if (!force && g_queuedPaths.count(key)) {
        LOGI("tray: %s was already queued -- skipping the duplicate", elog::Narrow(mp4).c_str());
        return;
    }
    g_queuedPaths.insert(key);
    if (g_pp.Enqueue(mp4)) LOGI("tray: queued %s for conversion", elog::Narrow(mp4).c_str());
    else LOGI("tray: %s is already queued / running", elog::Narrow(mp4).c_str());
}

// Fallback for a "saved <file>" message that never reached us (the game quit or crashed right after / during the
// recording): look for finished *_SBS.mp4 / *_VR180.mp4 / *_VR360.mp4 files in the captures folder that this tray
// session has not handled.
// Candidates: newer than the tray start, no sibling .mkv (or _FLAT.mp4 when only the flat step is on), not queued
// before. PollPending() then waits until the file is not open for writing and its size has been stable for 3 s.
void ScanCaptures() {
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    if (!cfg.postMkv && !cfg.postFlat) return;
    std::wstring dir = g_st.outputDir[0] ? std::wstring(g_st.outputDir) : cfg.outputDir;
    if (dir.empty()) dir = g_exeDir + L"\\captures";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.mp4").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    auto endsWith = [](const std::wstring& n, const wchar_t* tail) {
        const size_t k = wcslen(tail);
        return n.size() >= k && _wcsicmp(n.c_str() + n.size() - k, tail) == 0;
    };
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring n = fd.cFileName;
        if (!endsWith(n, L"_SBS.mp4") && !endsWith(n, L"_VR180.mp4") && !endsWith(n, L"_VR360.mp4")) continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &g_trayStart) <= 0) continue;
        const std::wstring path = dir + L"\\" + n;
        const std::wstring key = NormPath(path);
        if (g_queuedPaths.count(key) || g_pending.count(key)) continue;
        const std::wstring stem = path.substr(0, path.size() - 4);
        if (Exists(stem + L".mkv")) continue;
        if (!cfg.postMkv && Exists(stem + L"_FLAT.mp4")) continue;
        Pending p;
        p.path = path;
        p.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        p.stableSince = GetTickCount64();
        g_pending[key] = p;
        LOGI("tray: fallback scan found %s (waiting for it to settle)", elog::Narrow(path).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void PollPending() {
    if (g_pending.empty()) return;
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        Pending& p = it->second;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (g_queuedPaths.count(it->first) || !GetFileAttributesExW(p.path.c_str(), GetFileExInfoStandard, &a) ||
            Exists(p.path.substr(0, p.path.size() - 4) + L".mkv")) {
            it = g_pending.erase(it);   // handled by the status path / converted / deleted meanwhile
            continue;
        }
        const ULONGLONG size = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        if (size != p.size) { p.size = size; p.stableSince = now; ++it; continue; }
        if (now - p.stableSince < 3000) { ++it; continue; }
        // open for writing anywhere? (a read open that does not share write fails while a writer holds the file)
        HANDLE f = CreateFileW(p.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { p.stableSince = now; ++it; continue; }
        CloseHandle(f);
        LOGI("tray: fallback scan queued %s", elog::Narrow(p.path).c_str());
        const std::wstring path = p.path;
        it = g_pending.erase(it);
        QueueRecording(path, false);
    }
}

void Refresh() {
    g_live = false;
    const bool read = g_shm && EyeCapReadStatus(g_shm, g_st);
    if (read && g_st.ownerPid && EyeCapProcessAlive(g_st.ownerPid) && GetTickCount64() - g_st.heartbeatTick < 3000)
        g_live = true;
    const uint32_t state = g_live ? g_st.state : ECS_NONE;

    // A recording just finished: the layer reports "saved <file>" (it stays in the block even after the game exits).
    // The first read only sets the baseline, so a recording that finished before the tray started is not converted.
    if (read) {
        const bool saved = wcsncmp(g_st.message, L"saved ", 6) == 0 && g_st.file[0];
        if (!g_baselineDone) {
            g_baselineDone = true;
            if (saved) g_lastSaved = g_st.file;
        } else if (saved && g_lastSaved != g_st.file) {
            g_lastSaved = g_st.file;
            QueueRecording(g_lastSaved, false);
        }
    }

    // fallback scan: when the game just went away, and every 10 s while no recording is running
    {
        const ULONGLONG now = GetTickCount64();
        const bool recActive = g_live && (state == ECS_RECORDING || state == ECS_STARTING || state == ECS_STOPPING);
        const bool wentAway = g_wasLive && !g_live;
        if (wentAway || (!recActive && now - g_lastScan >= 10000)) {
            g_lastScan = now;
            ScanCaptures();
        }
        g_wasLive = g_live;
        PollPending();
    }

    const pp::State job = g_pp.GetState();
    int idx = 0;
    if (state == ECS_READY) idx = 1;
    else if (state == ECS_RECORDING) idx = 2;
    else if (state == ECS_STARTING || state == ECS_STOPPING || state == ECS_FAILED) idx = 3;
    if (job.busy && idx != 2 && idx != 3) idx = 4;   // converting (red recording / amber transitions keep priority)

    wchar_t tip[256];
    if (!g_live) {
        swprintf_s(tip, L"OpenXR-EyeCapture: no OpenXR game running\n(games started after this app are recorded)");
    } else if (state == ECS_RECORDING) {
        const ULONGLONG el = g_st.recStartTick ? (GetTickCount64() - g_st.recStartTick) / 1000 : 0;
        swprintf_s(tip, L"REC %llu:%02llu  %s (%s)\n%ux%u @%u fps, dropped %llu", el / 60, el % 60, g_st.game, g_st.api,
                   g_st.width, g_st.height, g_st.fps, (unsigned long long)g_st.framesDropped);
    } else if (state == ECS_STOPPING) {
        swprintf_s(tip, L"%s: finalizing the MP4...", g_st.game);
    } else if (state == ECS_FAILED) {
        swprintf_s(tip, L"%s: start failed\n%.80s", g_st.game, g_st.message);
    } else {
        swprintf_s(tip, L"%s (%s): ready\n%s toggles recording", g_st.game, g_st.api, g_st.hotkey);
    }
    // conversion line: "converting <file> (MKV)..." while a job runs, "last: <file>.mkv ready" after
    std::wstring jobLine;
    if (job.busy) {
        jobLine = L"converting " + job.file + L" (" + job.step + L")...";
        if (job.queued) jobLine += L" +" + std::to_wstring(job.queued) + L" queued";
    } else if (!job.lastLine.empty()) {
        jobLine = job.lastLine;
    }
    static std::wstring s_prevJobLine;
    if (jobLine != s_prevJobLine) { s_prevJobLine = jobLine; LOGI("tray: tooltip job line: %s", elog::Narrow(jobLine).c_str()); }
    if (idx != g_iconIdx && (idx == 4 || g_iconIdx == 4)) LOGI("tray: icon %s", idx == 4 ? "blue (converting)" : "no longer blue");
    wchar_t full[256];
    if (jobLine.empty()) wcsncpy_s(full, tip, _TRUNCATE);
    else if (state == ECS_RECORDING) swprintf_s(full, L"%s\n%s", tip, jobLine.c_str());   // recording info first
    else swprintf_s(full, L"%s\n%s", jobLine.c_str(), tip);
    g_nid.uFlags = NIF_TIP | NIF_ICON;
    wcsncpy_s(g_nid.szTip, full, _TRUNCATE);     // szTip holds 127 characters
    g_nid.hIcon = g_icons[idx];
    // balloon: a conversion result wins over "Recording saved"
    pp::Notice notice;
    if (g_pp.PopNotice(notice)) {
        g_nid.uFlags |= NIF_INFO;
        wcsncpy_s(g_nid.szInfoTitle, notice.title.c_str(), _TRUNCATE);
        wcsncpy_s(g_nid.szInfo, notice.text.c_str(), _TRUNCATE);
        g_nid.dwInfoFlags = notice.error ? NIIF_ERROR : NIIF_INFO;
        LOGI("tray: balloon [%s] %s", elog::Narrow(notice.title).c_str(), elog::Narrow(notice.text).c_str());
    } else if (g_live && g_prevState == ECS_STOPPING && state == ECS_READY && g_st.message[0]) {
        g_nid.uFlags |= NIF_INFO;
        wcsncpy_s(g_nid.szInfoTitle, L"Recording saved", _TRUNCATE);
        wcsncpy_s(g_nid.szInfo, g_st.message, _TRUNCATE);
        g_nid.dwInfoFlags = NIIF_INFO;
    }
    g_prevState = state;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_iconIdx = idx;
}

void OpenPath(const std::wstring& p, const wchar_t* verb = L"open") {
    if (!p.empty()) ShellExecuteW(nullptr, verb, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowMenu() {
    Refresh();
    HMENU m = CreatePopupMenu();
    const uint32_t state = g_live ? g_st.state : ECS_NONE;
    const bool rec = state == ECS_RECORDING || state == ECS_STARTING;
    std::wstring label = rec ? L"Stop recording" : L"Start recording";
    if (g_live && g_st.hotkey[0]) label += std::wstring(L"\t") + g_st.hotkey;
    AppendMenuW(m, MF_STRING | ((g_live && state != ECS_STOPPING) ? 0 : MF_GRAYED), ID_TOGGLE, label.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Open captures folder");
    AppendMenuW(m, MF_STRING, ID_CONVERT, L"Convert last recording now");
    AppendMenuW(m, MF_STRING, ID_LOG, L"Open log");
    AppendMenuW(m, MF_STRING, ID_CONFIG, L"Edit config");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    Config cfg;
    switch (cmd) {
    case ID_TOGGLE:
        if (g_shm) EyeCapSendCommand(g_shm, rec ? ECC_STOP : ECC_START);
        break;
    case ID_FOLDER: {
        std::wstring dir = g_live ? g_st.outputDir : L"";
        if (dir.empty()) { LoadConfig(g_exeDir, cfg); dir = cfg.outputDir; }
        CreateDirectoryW(dir.c_str(), nullptr);
        OpenPath(dir, L"explore");
        break;
    }
    case ID_CONVERT: {
        // The most recent recording: the last one this tray saw / converted, else the newest *.mp4 in the captures folder.
        std::wstring file = g_pp.LastQueued();
        if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring dir = g_live ? g_st.outputDir : L"";
            if (dir.empty()) { LoadConfig(g_exeDir, cfg); dir = cfg.outputDir; }
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*.mp4").c_str(), &fd);
            FILETIME newest{};
            file.clear();
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    const std::wstring n = fd.cFileName;
                    if (n.size() >= 9 && _wcsicmp(n.c_str() + n.size() - 9, L"_FLAT.mp4") == 0) continue;
                    if (file.empty() || CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) { file = dir + L"\\" + n; newest = fd.ftLastWriteTime; }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LOGI("tray: convert now: no MP4 found to convert");
            g_nid.uFlags = NIF_INFO;
            wcsncpy_s(g_nid.szInfoTitle, L"Nothing to convert", _TRUNCATE);
            wcsncpy_s(g_nid.szInfo, L"No MP4 found. (Converted recordings have their MP4 removed unless keep_mp4 = 1.)", _TRUNCATE);
            g_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        } else {
            LOGI("tray: convert now requested for %s", elog::Narrow(file).c_str());
            QueueRecording(file, true);
        }
        break;
    }
    case ID_LOG:
        OpenPath(g_live && g_st.logPath[0] ? std::wstring(g_st.logPath) : g_exeDir + L"\\logs\\eye_capture.log");
        break;
    case ID_CONFIG: {
        const std::wstring p = g_live && g_st.configPath[0] ? std::wstring(g_st.configPath) : ConfigPath(g_exeDir);
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case ID_EXIT:
        DestroyWindow(g_wnd);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == s_taskbarCreated) { Shell_NotifyIconW(NIM_ADD, &g_nid); return 0; }
    switch (msg) {
    case WM_TIMER:
    case WM_PP:
        Refresh();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        return 0;
    case WM_DESTROY:
        KillTimer(h, kTimer);
        g_pp.Stop();   // cancels a running ffmpeg and deletes its partial output
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}
// Command-line mode (no tray): EyeCaptureTray.exe --start | --stop | --toggle | --status
// For scripts, Stream Deck buttons, and tests. Prints one status line; exit code 0 = a live game received the command.
int RunCli(const std::wstring& arg) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto print = [&](const std::wstring& s) {
        if (!out || out == INVALID_HANDLE_VALUE) return;
        const std::string u = elog::Narrow(s + L"\r\n");
        DWORD w = 0;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if ((arg == L"--start" || arg == L"--toggle") && !EyeCapTrayRunning()) {
        print(L"error: EyeCaptureTray is not running — start it before the game so the layer is active");
        return 4;
    }
    HANDLE map = nullptr;
    EyeCapShared* shm = EyeCapOpenShared(&map);
    if (!shm) { print(L"error: shared memory unavailable"); return 3; }
    int rc = 0;
    if (arg != L"--status") {
        const uint32_t cmd = arg == L"--start" ? ECC_START : arg == L"--stop" ? ECC_STOP : ECC_TOGGLE;
        if (!EyeCapSendCommand(shm, cmd)) { print(L"no OpenXR game with a capture-capable session is running"); rc = 1; }
        else Sleep(600);   // let the layer react before printing the status
    }
    EyeCapShared st{};
    const bool live = EyeCapReadStatus(shm, st) && st.ownerPid && EyeCapProcessAlive(st.ownerPid);
    static const wchar_t* names[] = {L"none", L"ready", L"starting", L"recording", L"stopping", L"failed"};
    wchar_t line[900];
    if (!live) {
        swprintf_s(line, L"state=none (no game)");
        if (arg == L"--status") rc = 1;
    } else {
        swprintf_s(line, L"state=%s pid=%u game=%s api=%s size=%ux%u fps=%u frames=%llu dropped=%llu file=%s msg=%s",
                   st.state < 6 ? names[st.state] : L"?", st.ownerPid, st.game, st.api, st.width, st.height, st.fps,
                   (unsigned long long)st.framesWritten, (unsigned long long)st.framesDropped, st.file, st.message);
    }
    print(line);
    UnmapViewOfFile(shm);
    CloseHandle(map);
    return rc;
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && cmdLine[0] == L'-') {
        std::wstring a = cmdLine;
        while (!a.empty() && a.back() == L' ') a.pop_back();
        if (a == L"--start" || a == L"--stop" || a == L"--toggle" || a == L"--status") return RunCli(a);
    }
    // Test-only: EYECAP_TRAY_TEST_SHM=<name> runs an extra tray instance on a private status block (no single-instance
    // check, no tray-alive marker), so tests/status_sim.exe can drive it without touching a real game's status.
    wchar_t testShm[128];
    const bool testMode = GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_SHM", testShm, 128) > 0;
    HANDLE single = nullptr;
    if (!testMode) {
        single = CreateMutexW(nullptr, TRUE, L"Local\\OpenXR-EyeCapture-Tray");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    }
    // "tray is alive" marker the layer looks for (GUI mode only; the CLI modes never create it). Null DACL so a game
    // running at a different integrity level can still open it.
    SECURITY_DESCRIPTOR sd;
    SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    HANDLE alive = testMode ? nullptr : CreateMutexW(&sa, FALSE, EYECAP_TRAY_ALIVE_NAME);
    g_inst = inst;
    g_exeDir = ExeDir();
    GetSystemTimeAsFileTime(&g_trayStart);
    elog::Init(g_exeDir + L"\\logs\\tray.log", false);
    LOGI("tray: started%s (exe folder %s)", testMode ? " [TEST MODE, private status block]" : "", elog::Narrow(g_exeDir).c_str());
    g_shm = testMode ? EyeCapOpenShared(&g_map, testShm) : EyeCapOpenShared(&g_map);
    g_icons[0] = MakeDotIcon(RGB(140, 140, 140));
    g_icons[1] = MakeDotIcon(RGB(40, 190, 70));
    g_icons[2] = MakeDotIcon(RGB(230, 30, 30));
    g_icons[3] = MakeDotIcon(RGB(240, 170, 20));
    g_icons[4] = MakeDotIcon(RGB(50, 130, 240));

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"OpenXREyeCaptureTray";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(wc.lpszClassName, L"OpenXR-EyeCapture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icons[0];
    wcsncpy_s(g_nid.szTip, L"OpenXR-EyeCapture", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    SetTimer(g_wnd, kTimer, 500, nullptr);
    g_pp.Start(g_exeDir, g_wnd, WM_PP);
    {
        // resolve ffmpeg once at start so the log says what a job would use (jobs resolve again when they run)
        Config cfg;
        LoadConfig(g_exeDir, cfg);
        std::wstring how;
        const std::wstring ff = pp::ResolveFfmpeg(cfg, g_exeDir, how);
        if (ff.empty()) LOGW("tray: ffmpeg not found at start (%s)", elog::Narrow(how).c_str());
        else LOGI("tray: ffmpeg = %s (%s)", elog::Narrow(ff).c_str(), elog::Narrow(how).c_str());
    }
    Refresh();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (HICON i : g_icons) if (i) DestroyIcon(i);
    if (g_shm) UnmapViewOfFile(g_shm);
    if (g_map) CloseHandle(g_map);
    if (alive) CloseHandle(alive);
    if (single) CloseHandle(single);
    return 0;
}
