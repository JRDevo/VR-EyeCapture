// EyeCaptureTray -- tray control for the OpenXR-EyeCapture layer (Win32, no frameworks). The layer is dormant (pure
// pass-through) unless this app is running: it holds the named mutex Local\OpenXR-EyeCapture-TrayAlive while it runs.
//
// Tray icon: grey = no game, green = game running (ready), red = recording, amber = starting / finalizing / failed,
// blue = converting a finished recording (post-processing, see tray/postprocess.h).
// Right-click (or left-click) menu: Start / Stop recording, Open captures folder, Convert last recording now, Open log,
// Settings..., Edit config, Register / Unregister for VR games..., Exit.
// Registration (tray/install.h): at start the tray removes its own HKCU duplicates and checks HKLM; when the layer is
// not registered for this folder it offers to register it and runs itself elevated (one UAC prompt) with --register.
// Tooltip: game, API, resolution, fps, frames dropped, elapsed, conversion progress. Talks to the layer through
// src/shared_status.h. After each recording the tray remuxes it to .mkv (YouTube 3D tag) in a worker thread.
// post_when (config.ini, re-read when the file changes): now = convert at once; session_end = hold the conversion queue
// while an OpenXR session is live (the layer releases the status block -- ownerPid 0, ECS_NONE -- when the game destroys
// its last session, even if the process keeps running); game_exit (default) = also hold while the process that made a
// queued recording is still alive. See UpdateHold() and pp::Processor::SetHold.
// Start it BEFORE the game: the layer decides at xrCreateSession whether to arm (config require_tray=0 = always arm).
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "config.h"
#include "input.h"
#include "install.h"
#include "postprocess.h"
#include "settings.h"
#include "shared_status.h"

namespace {
const UINT WM_TRAY = WM_APP + 1;
const UINT WM_PP = WM_APP + 2;     // the post-processing worker changed state / queued a notice
const UINT WM_OPEN_SETTINGS = WM_APP + 3;   // also posted by tests/settings_test.exe to open the Settings window
const UINT WM_CHECK_REG = WM_APP + 4;       // posted once at start: registration prompt (inside the message loop)
const UINT kTimer = 1;
const UINT kTipTimer = 2;     // one-shot: end of the "settings saved" tooltip
enum : UINT { ID_TOGGLE = 100, ID_FOLDER, ID_CONVERT, ID_LOG, ID_SETTINGS, ID_CONFIG, ID_EXIT, ID_REGISTER, ID_UNREGISTER };
const wchar_t kAppTitle[] = L"OpenXR-EyeCapture";

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
bool           g_testMode = false;
std::wstring   g_savedTip;                         // temporary tooltip after Settings -> Save
ULONGLONG      g_savedTipUntil = 0;
ULONGLONG      g_lastScan = 0;
// Registration of the layer (tray/install.h). Off in the EYECAP_TRAY_TEST_SHM test mode unless EYECAP_TRAY_TEST_REG=1.
bool           g_regEnabled = false;
install::Status g_reg;                             // cached; re-queried every 10 s and after a register / unregister
ULONGLONG      g_regQueried = 0;
bool           g_regBusy = false;                  // a registration prompt / elevated child is in progress
// post_when (tray/postprocess.h hold). Producers: the game processes that made a queued recording (game_exit waits for
// them to exit). Handles, not pids, so a re-used pid can never keep the queue held.
PostWhen       g_postWhen = PostWhen::GameExit;    // cached; reloaded with the input bindings when config.ini changes
struct Proc { DWORD pid = 0; HANDLE h = nullptr; };
Proc           g_lastOwner;                        // the last live owner of the status block (ownerPid is 0 once released)
std::vector<Proc> g_producers;

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

// ---- post_when: hold the conversion queue while the game / its VR session is still running
const char* PostWhenName(PostWhen w) { return w == PostWhen::Now ? "now" : w == PostWhen::SessionEnd ? "session_end" : "game_exit"; }

bool ProcAlive(HANDLE h) { return h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT; }

// Remembers the live owner of the status block (the layer clears ownerPid when it releases the block, so it is cached).
void TrackOwner(DWORD pid) {
    if (!pid || pid == g_lastOwner.pid) return;
    if (g_lastOwner.h) CloseHandle(g_lastOwner.h);
    g_lastOwner.pid = pid;
    g_lastOwner.h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

// The recording being queued was made by the last live owner: game_exit waits for that process.
void AddProducer() {
    if (!ProcAlive(g_lastOwner.h)) return;
    for (const Proc& p : g_producers) if (p.pid == g_lastOwner.pid) return;
    Proc p;
    p.pid = g_lastOwner.pid;
    if (!DuplicateHandle(GetCurrentProcess(), g_lastOwner.h, GetCurrentProcess(), &p.h, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    g_producers.push_back(p);
    LOGI("tray: recording made by pid %lu (post_when = game_exit waits for that process to exit)", (unsigned long)p.pid);
}

// Drops exited producers; true while one is still alive.
bool ProducersAlive() {
    for (auto it = g_producers.begin(); it != g_producers.end();) {
        if (ProcAlive(it->h)) { ++it; continue; }
        LOGI("tray: pid %lu (made a queued recording) has exited", (unsigned long)it->pid);
        CloseHandle(it->h);
        it = g_producers.erase(it);
    }
    return !g_producers.empty();
}

void ClearProducers() {
    for (Proc& p : g_producers) CloseHandle(p.h);
    g_producers.clear();
}

const wchar_t* HoldReason() { return g_postWhen == PostWhen::SessionEnd ? L"when VR stops" : L"when the game closes"; }

// Computes the hold from post_when and the live status and hands it to the worker (it logs the on / off transitions).
// Called every tick and right before a recording is queued, so a job can never start in the gap.
void UpdateHold() {
    const bool sessionLive = g_live && g_st.state != ECS_NONE;
    bool hold = false;
    if (g_postWhen == PostWhen::SessionEnd) hold = sessionLive;
    else if (g_postWhen == PostWhen::GameExit) hold = ProducersAlive() || sessionLive;
    g_pp.SetHold(hold, HoldReason());
}

// Queue a finished recording for post-processing (MP4 only; a no-op when post_mkv, post_flat and post_yt8k are all 0).
// Non-forced requests are deduplicated by full path for the whole tray session (status message vs fallback scan).
// Forced requests ("Convert last recording now") ignore post_when: they run at once.
void QueueRecording(const std::wstring& mp4, bool force) {
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    if (!force && !cfg.postMkv && !cfg.postFlat && !cfg.postYt8k) {
        LOGI("tray: recording %s saved; post_mkv, post_flat and post_yt8k are all 0 -- not converting", elog::Narrow(mp4).c_str());
        return;
    }
    const std::wstring key = NormPath(mp4);
    if (!force && g_queuedPaths.count(key)) {
        LOGI("tray: %s was already queued -- skipping the duplicate", elog::Narrow(mp4).c_str());
        return;
    }
    g_queuedPaths.insert(key);
    if (!force) {
        AddProducer();
        UpdateHold();   // before Enqueue: the worker must see the hold before it can pick the job
    }
    if (g_pp.Enqueue(mp4, force)) LOGI("tray: queued %s for conversion%s", elog::Narrow(mp4).c_str(), force ? " (forced: ignores post_when)" : "");
    else LOGI("tray: %s is already queued / running", elog::Narrow(mp4).c_str());
}

// Fallback for a "saved <file>" message that never reached us (the game quit or crashed right after / during the
// recording): look for finished *_SBS.mp4 / *_VR180.mp4 / *_VR360.mp4 files in the captures folder that this tray
// session has not handled.
// Candidates: newer than the tray start, no sibling .mkv (or _FLAT.mp4 / _YT8K.mkv when the MKV step is off), not
// queued before. PollPending() then waits until the file is not open for writing and its size has been stable for 3 s.
void ScanCaptures() {
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    if (!cfg.postMkv && !cfg.postFlat && !cfg.postYt8k) return;
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
        if (!cfg.postMkv && (Exists(stem + L"_FLAT.mp4") || Exists(stem + L"_YT8K.mkv"))) continue;
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

// ---- registration (tray/install.h) ----
bool RegOk() { return g_reg.state == install::State::Registered || g_reg.state == install::State::DuplicateHkcu; }

void LogRegStatus(const char* prefix) {
    LOGI("tray: %s: %s%s%s%s%s", prefix, install::StateText(g_reg.state), g_reg.registeredPath.empty() ? "" : " (",
         elog::Narrow(g_reg.registeredPath).c_str(), g_reg.registeredPath.empty() ? "" : ")",
         g_reg.filesPresent ? "" : "; eye_capture.dll / json NOT next to the exe");
}

// Cheap registry read. Logs only when the state changed (or always when `force`).
void QueryReg(bool force) {
    const install::Status prev = g_reg;
    g_reg = install::Query(g_exeDir);
    g_regQueried = GetTickCount64();
    if (force || g_reg.state != prev.state || g_reg.registeredPath != prev.registeredPath ||
        g_reg.filesPresent != prev.filesPresent || g_reg.hkcuDuplicate != prev.hkcuDuplicate)
        LogRegStatus("registration");
}

// Tests: EYECAP_TRAY_TEST_REG_ROOTS=<lm key>;<cu key> (both under HKCU\) replaces HKLM / HKCU in tray/install.cpp, so
// tests/run_install.ps1 can drive --register / --unregister / --status and the GUI flow without admin rights.
void ApplyTestRegRoots() {
    wchar_t v[512];
    const DWORD n = GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_REG_ROOTS", v, 512);
    if (n == 0 || n >= 512) return;
    const std::wstring s = v;
    const size_t semi = s.find(L';');
    if (semi == std::wstring::npos) return;
    const std::wstring lm = s.substr(0, semi), cu = s.substr(semi + 1);
    if (_wcsnicmp(lm.c_str(), L"HKCU\\", 5) != 0 || _wcsnicmp(cu.c_str(), L"HKCU\\", 5) != 0) return;   // never a real HKLM key
    install::SetTestRoots(lm, cu);
    LOGI("tray: TEST registry roots: LM -> %s, CU -> %s", elog::Narrow(lm).c_str(), elog::Narrow(cu).c_str());
}

// Tests: EYECAP_TRAY_TEST_RUN_KEY=HKCU\<subkey> redirects the "Start with Windows" Run key (tray/install.h). A test-mode tray
// (EYECAP_TRAY_TEST_SHM) without it uses Software\OpenXR-EyeCapture-Test\Run anyway, so a test never touches the real Run key.
void ApplyTestRunKey(bool testMode) {
    wchar_t v[512];
    const DWORD n = GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_RUN_KEY", v, 512);
    if (n > 0 && n < 512) {
        const std::wstring s = v;
        if (s.size() > 5 && _wcsnicmp(s.c_str(), L"HKCU\\", 5) == 0) {
            install::SetAutostartTestKey(s.substr(5));
            return;
        }
    }
    if (testMode) install::SetAutostartTestKey(L"Software\\OpenXR-EyeCapture-Test\\Run");
}

void Refresh() {
    g_live = false;
    const bool read = g_shm && EyeCapReadStatus(g_shm, g_st);
    if (read && g_st.ownerPid && EyeCapProcessAlive(g_st.ownerPid) && GetTickCount64() - g_st.heartbeatTick < 3000)
        g_live = true;
    const uint32_t state = g_live ? g_st.state : ECS_NONE;
    if (g_live) TrackOwner(g_st.ownerPid);
    {
        // nothing queued or running: forget the producers (a later recording registers its own)
        const pp::State js = g_pp.GetState();
        if (!js.busy && js.queued == 0 && !g_producers.empty()) ClearProducers();
    }
    UpdateHold();

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
    const bool waiting = job.held && job.queued > 0;   // queued, held by post_when
    if (job.busy && !job.paused && idx != 2 && idx != 3) idx = 4;   // converting (red recording / amber transitions keep priority)
    else if (idx == 0 && (job.busy || waiting)) idx = 4;            // paused / waiting while no game is live: still blue

    // not registered: the first tooltip line says so (the icon is grey anyway: no game can load the layer from here)
    const bool notReg = g_regEnabled && !RegOk();
    wchar_t tip[256];
    if (!g_live && notReg) {
        swprintf_s(tip, L"OpenXR-EyeCapture: no OpenXR game running");   // "(games started after ...)" would be untrue
    } else if (!g_live) {
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
    if (job.busy && job.paused) {
        jobLine = L"conversion paused: " + job.file + L" -- resumes " + job.holdReason;
        if (job.queued) jobLine += L" (+" + std::to_wstring(job.queued) + L" waiting)";
    } else if (job.busy) {
        jobLine = L"converting " + job.file + L" (" + job.step + L")...";
        if (job.queued) jobLine += L" +" + std::to_wstring(job.queued) + (job.held ? L" waiting" : L" queued");
    } else if (waiting) {
        jobLine = std::to_wstring(job.queued) + (job.queued == 1 ? L" recording waiting" : L" recordings waiting") +
                  L" -- converts " + job.holdReason;
    } else if (!job.lastLine.empty()) {
        jobLine = job.lastLine;
    }
    static std::wstring s_prevJobLine;
    if (jobLine != s_prevJobLine) { s_prevJobLine = jobLine; LOGI("tray: tooltip job line: %s", elog::Narrow(jobLine).c_str()); }
    if (idx != g_iconIdx && (idx == 4 || g_iconIdx == 4))
        LOGI("tray: icon %s", idx == 4 ? (job.busy && !job.paused ? "blue (converting)" : "blue (waiting to convert)") : "no longer blue");
    wchar_t full[256];
    if (jobLine.empty()) wcsncpy_s(full, tip, _TRUNCATE);
    else if (state == ECS_RECORDING) swprintf_s(full, L"%s\n%s", tip, jobLine.c_str());   // recording info first
    else swprintf_s(full, L"%s\n%s", jobLine.c_str(), tip);
    if (notReg) {
        const std::wstring withReg = std::wstring(L"Not registered for VR games (right-click: Register)\n") + full;
        wcsncpy_s(full, withReg.c_str(), _TRUNCATE);
    }
    static int s_prevNotReg = -1;
    const bool logRegTip = s_prevNotReg != (int)notReg && g_regEnabled;
    s_prevNotReg = (int)notReg;
    if (!g_savedTip.empty() && GetTickCount64() < g_savedTipUntil) wcsncpy_s(full, g_savedTip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP | NIF_ICON;
    wcsncpy_s(g_nid.szTip, full, _TRUNCATE);     // szTip holds 127 characters
    if (logRegTip) LOGI("tray: tooltip now: %s", elog::Narrow(g_nid.szTip).c_str());
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

std::wstring SettingsPath() {
    return g_live && g_st.configPath[0] ? std::wstring(g_st.configPath) : ConfigPath(g_exeDir);
}

// ---- controller / wheel hotkeys (tray/input.h): the Pad: / Joy: bindings of `hotkey`, fired by the tray itself
std::wstring   g_cfgSeenPath;          // config file + write time the bindings were last loaded from
FILETIME       g_cfgSeenTime{};

// Poll thread of tray/input.cpp. The main thread only reads the status block; sending a command is safe from here.
void OnInputFire(const HotkeyBinding& b) {
    EyeCapShared* shm = g_shm;
    if (shm && EyeCapProcessAlive(shm->ownerPid) && EyeCapSendCommand(shm, ECC_TOGGLE))
        LOGI("tray: input %s pressed -> toggle", elog::Narrow(b.text).c_str());
    else
        LOGI("tray: input %s pressed but no game is running", elog::Narrow(b.text).c_str());
}

FILETIME ConfigWriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return FILETIME{};
    return a.ftLastWriteTime;
}

// (Re)loads config.ini and hands its Pad: / Joy: bindings to the input module (they apply at once; keyboard / mouse
// bindings are the layer's and need a game restart).
void ApplyInputBindings(const char* why) {
    g_cfgSeenPath = ConfigPath(g_exeDir);
    g_cfgSeenTime = ConfigWriteTime(g_cfgSeenPath);
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    input::SetBindings(cfg.hotkey.bindings);
    static bool s_postWhenLogged = false;
    if (!s_postWhenLogged || cfg.postWhen != g_postWhen) {
        s_postWhenLogged = true;
        g_postWhen = cfg.postWhen;
        LOGI("tray: post_when = %s (%s)", PostWhenName(g_postWhen), g_postWhen == PostWhen::Now ? "convert recordings right away"
             : g_postWhen == PostWhen::SessionEnd ? "convert when no VR session is live" : "convert when the game that made the recording has closed");
    }
    std::wstring list;
    for (const auto& b : cfg.hotkey.bindings)
        if (b.kind != HotkeyBinding::Kind::Key) list += (list.empty() ? L"" : L", ") + b.text;
    LOGI("tray: hotkey %s (%s); controller / wheel bindings: %s", elog::Narrow(cfg.hotkey.text).c_str(), why,
         list.empty() ? "none" : elog::Narrow(list).c_str());
}

// Cheap check in the 500 ms timer: config.ini edited by hand (or the %APPDATA% copy appeared / went away).
void CheckConfigChanged() {
    const std::wstring p = ConfigPath(g_exeDir);
    const FILETIME t = ConfigWriteTime(p);
    if (p == g_cfgSeenPath && CompareFileTime(&t, &g_cfgSeenTime) == 0) return;
    ApplyInputBindings("config.ini changed");
}

// Settings -> Save: log the changed keys, show "settings saved" in the tooltip for 5 s.
void OnSettingsSaved(const std::vector<std::wstring>& changed) {
    {
        // apply log_level now (e.g. "none" silences tray.log without a restart); the saved file passed validation
        Config c;
        std::vector<std::wstring> problems;
        ParseConfigFile(SettingsPath(), c, problems);
        elog::SetLevel(c.logLevel);
    }
    std::wstring list;
    for (const auto& c : changed) list += (list.empty() ? L"" : L", ") + c;
    LOGI("tray: settings saved to %s: %s", elog::Narrow(SettingsPath()).c_str(),
         changed.empty() ? "no changes" : ("changed " + elog::Narrow(list)).c_str());
    ApplyInputBindings("settings saved");   // controller / wheel hotkeys apply at once
    g_savedTip = L"settings saved — applies to the next recording";
    g_savedTipUntil = GetTickCount64() + 5000;
    SetTimer(g_wnd, kTipTimer, 5100, nullptr);
    Refresh();
    LOGI("tray: tooltip now: %s", elog::Narrow(g_nid.szTip).c_str());
}

void SettingsReporter(const std::wstring& title, const std::wstring& text) {
    LOGI("tray: settings message box [%s]: %s", elog::Narrow(title).c_str(), elog::Narrow(text).c_str());
}

void OpenSettings() {
    if (settings::DialogWindow()) { settings::ShowDialog(L"", nullptr); return; }
    settings::ShowDialog(SettingsPath(), OnSettingsSaved);
}

// ---- registration UI ----
void Balloon(const std::wstring& title, const std::wstring& text, bool error) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    LOGI("tray: balloon [%s] %s", elog::Narrow(title).c_str(), elog::Narrow(text).c_str());
}

// Yes / No question. Test mode (EYECAP_TRAY_TEST_SHM): logged and answered No, so tests never block on a dialog and
// never reach the UAC prompt.
bool Ask(const std::wstring& text) {
    if (g_testMode) {
        LOGI("tray: question box: %s -> No (test mode)", elog::Narrow(text).c_str());
        return false;
    }
    const int r = MessageBoxW(nullptr, text.c_str(), kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    LOGI("tray: question box answered %s: %s", r == IDYES ? "Yes" : "No", elog::Narrow(text).c_str());
    return r == IDYES;
}

void ShowError(const std::wstring& text) {
    LOGE("tray: error box: %s", elog::Narrow(text).c_str());
    if (!g_testMode) MessageBoxW(nullptr, text.c_str(), kAppTitle, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// Runs this exe elevated ("runas" = the UAC prompt) with `arg` and waits for it (messages keep flowing so the tray
// stays alive). Returns its exit code, or -1 when it was not started (launchErr: ERROR_CANCELLED = UAC declined).
int Elevate(const wchar_t* arg, DWORD& launchErr) {
    launchErr = 0;
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    if (n == 0 || n >= sizeof(exe) / sizeof(exe[0])) { launchErr = GetLastError(); return -1; }
    LOGI("tray: starting elevated: \"%s\" %s", elog::Narrow(exe).c_str(), elog::Narrow(arg).c_str());
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // ShellExecuteEx wants COM
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = arg;
    sei.lpDirectory = g_exeDir.c_str();
    sei.nShow = SW_HIDE;
    const BOOL ok = ShellExecuteExW(&sei);
    launchErr = ok ? 0 : GetLastError();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) { LOGW("tray: elevated start failed (error %lu)", launchErr); return -1; }
    if (!sei.hProcess) { launchErr = ERROR_INVALID_HANDLE; LOGW("tray: elevated start returned no process handle"); return -1; }
    const ULONGLONG until = GetTickCount64() + 120000;
    bool quit = false;
    WPARAM quitCode = 0;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= until) break;
        const DWORD r = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, (DWORD)(until - now), QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) break;   // process ended, timeout or failure
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quit = true; quitCode = m.wParam; continue; }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DWORD code = 1;
    if (WaitForSingleObject(sei.hProcess, 0) != WAIT_OBJECT_0) LOGW("tray: the elevated process did not finish within 120 s");
    else GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (quit) PostQuitMessage((int)quitCode);
    LOGI("tray: elevated %s exited with %lu", elog::Narrow(arg).c_str(), code);
    return (int)code;
}

std::wstring ElevatedFailText(const wchar_t* what, int code) {
    std::wstring s = std::wstring(what) + L" failed";
    if (code == 5) s += L": Windows refused administrator access to the registry.";
    else s += L" (exit code " + std::to_wstring(code) + L").";
    return s + L"\n\nSee logs\\tray.log next to EyeCaptureTray.exe for details.";
}

const wchar_t kFilesMissing[] = L"eye_capture.dll / json not found next to EyeCaptureTray.exe";

// The "Register for VR games" flow: question (UAC warning), elevated --register, result balloon / error box.
// fromStart = the prompt shown when the tray starts (mentions the tray menu for later).
void RegisterFlow(bool fromStart) {
    if (g_regBusy) return;
    g_regBusy = true;
    QueryReg(false);
    if (!g_reg.filesPresent) {
        LOGW("tray: register: %s (%s)", elog::Narrow(kFilesMissing).c_str(), elog::Narrow(g_exeDir).c_str());
        Balloon(kAppTitle, kFilesMissing, true);
        g_regBusy = false;
        return;
    }
    if (RegOk()) {   // registered meanwhile (another tray copy / an admin prompt): nothing to ask
        g_regBusy = false;
        Refresh();
        return;
    }
    std::wstring q;
    if (g_reg.state == install::State::StalePath) {
        q = L"OpenXR-EyeCapture is registered from another folder:\n" + g_reg.registeredPath +
            L"\n\nRe-register it from this folder? Windows will ask for administrator rights once.";
    } else {
        q = L"OpenXR-EyeCapture is not registered for VR games yet.\n\nRegister it now? Windows will ask for administrator "
            L"rights once.";
        if (fromStart) q += L"\n\n(Later: right-click the tray icon -> Register for VR games.)";
    }
    if (!Ask(q)) { g_regBusy = false; Refresh(); return; }
    DWORD launchErr = 0;
    const int code = Elevate(L"--register", launchErr);
    install::RemoveHkcuDuplicates();   // the elevated process may be another account: our own HKCU is cleaned here
    QueryReg(true);
    if (code < 0 && launchErr == ERROR_CANCELLED) {
        Balloon(kAppTitle, L"Registration cancelled. Use the tray menu when ready.", false);
    } else if (code < 0) {
        ShowError(L"Could not start EyeCaptureTray.exe with administrator rights (error " + std::to_wstring(launchErr) +
                  L").\n\nSee logs\\tray.log next to EyeCaptureTray.exe for details.");
    } else if (code != 0) {
        ShowError(ElevatedFailText(L"Registration", code));
    } else if (!RegOk()) {
        ShowError(L"Registration reported success, but the registry still says: " +
                  elog::Widen(install::StateText(g_reg.state)) + L".\n\nSee logs\\tray.log next to EyeCaptureTray.exe.");
    } else {
        Config cfg;
        LoadConfig(g_exeDir, cfg);
        if (!cfg.hotkey.bindings.empty() && !cfg.hotkey.text.empty())
            Balloon(L"Registered", L"Registered. Start a VR game and press " + cfg.hotkey.text + L" to record.", false);
        else
            Balloon(L"Registered", L"Registered. Start a VR game and use the tray menu to record.", false);
    }
    g_regBusy = false;
    Refresh();
}

void UnregisterFlow() {
    if (g_regBusy) return;
    g_regBusy = true;
    const std::wstring q = L"Unregister OpenXR-EyeCapture from VR games?\n\nGames will no longer load the recorder. Your "
                           L"files and recordings stay. Windows will ask for administrator rights once.";
    if (!Ask(q)) { g_regBusy = false; return; }
    DWORD launchErr = 0;
    const int code = Elevate(L"--unregister", launchErr);
    install::RemoveHkcuDuplicates();
    QueryReg(true);
    if (code < 0 && launchErr == ERROR_CANCELLED) {
        Balloon(kAppTitle, L"Unregistering cancelled. Nothing was changed.", false);
    } else if (code < 0) {
        ShowError(L"Could not start EyeCaptureTray.exe with administrator rights (error " + std::to_wstring(launchErr) +
                  L").\n\nSee logs\\tray.log next to EyeCaptureTray.exe for details.");
    } else if (code != 0) {
        ShowError(ElevatedFailText(L"Unregistering", code));
    } else {
        Balloon(L"Unregistered", L"Unregistered. VR games no longer load OpenXR-EyeCapture. Exit the tray app or "
                                 L"register again from its menu.", false);
    }
    g_regBusy = false;
    Refresh();
}

// Once at start (posted, so it runs inside the message loop with the icon already shown).
void StartupRegCheck() {
    if (!g_regEnabled) return;
    if (!g_reg.filesPresent) {
        LOGW("tray: %s (%s) -- no registration prompt", elog::Narrow(kFilesMissing).c_str(), elog::Narrow(g_exeDir).c_str());
        Balloon(kAppTitle, kFilesMissing, true);
        return;
    }
    if (g_reg.state == install::State::DuplicateHkcu)
        LOGW("tray: the layer is also registered in HKCU and that value could not be removed (games may load it twice)");
    if (!RegOk()) RegisterFlow(true);
}

void ShowMenu() {
    if (g_regBusy) return;   // a registration question / UAC prompt is open
    if (g_regEnabled) QueryReg(false);
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
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING, ID_CONFIG, L"Edit config");
    if (g_regEnabled) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (RegOk()) AppendMenuW(m, MF_STRING, ID_UNREGISTER, L"Unregister from VR games...");
        else AppendMenuW(m, MF_STRING, ID_REGISTER, L"Register for VR games...");
    }
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
    case ID_SETTINGS:
        OpenSettings();
        break;
    case ID_CONFIG: {
        const std::wstring p = SettingsPath();
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case ID_REGISTER:
        RegisterFlow(false);
        break;
    case ID_UNREGISTER:
        UnregisterFlow();
        break;
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
        if (wp == kTipTimer) { KillTimer(h, kTipTimer); g_savedTip.clear(); Refresh(); LOGI("tray: tooltip restored: %s", elog::Narrow(g_nid.szTip).c_str()); return 0; }
        if (g_regEnabled && !g_regBusy && GetTickCount64() - g_regQueried >= 10000) QueryReg(false);   // cheap registry read
        CheckConfigChanged();   // one GetFileAttributesEx: controller / wheel hotkeys follow config.ini edits
        Refresh();
        return 0;
    case WM_CHECK_REG:
        StartupRegCheck();
        return 0;
    case WM_PP:
        Refresh();
        return 0;
    case WM_OPEN_SETTINGS:
        OpenSettings();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        return 0;
    case WM_DESTROY:
        KillTimer(h, kTimer);
        g_pp.Stop();   // cancels a running ffmpeg (resumed first if held) and deletes its partial output
        ClearProducers();
        if (g_lastOwner.h) { CloseHandle(g_lastOwner.h); g_lastOwner = Proc(); }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}
// Prints one line to stdout (redirected) or to the parent's console (a GUI-subsystem exe has none of its own).
void CliPrint(const std::wstring& s) {
    static HANDLE out = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!out || out == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS))
                out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
    }
    if (!out || out == INVALID_HANDLE_VALUE) return;
    const std::string u = elog::Narrow(s + L"\r\n");
    DWORD w = 0;
    WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
}

// EyeCaptureTray.exe --register | --unregister: writes HKLM (needs admin; the GUI runs this elevated, or run it from an
// admin prompt). Logs to logs\tray.log. Exit code 0 = done, 5 = access denied, 1 = any other failure.
int RunRegCli(const std::wstring& arg) {
    const bool reg = arg == L"--register";
    g_exeDir = ExeDir();
    elog::Init(g_exeDir + L"\\logs\\tray.log", false);
    {
        Config cfg;
        LoadConfig(g_exeDir, cfg);
        elog::SetLevel(cfg.logLevel);
    }
    ApplyTestRegRoots();
    LOGI("tray: %s (exe folder %s)", elog::Narrow(arg).c_str(), elog::Narrow(g_exeDir).c_str());
    std::wstring err;
    long winErr = 0;
    const bool ok = reg ? install::Register(g_exeDir, err, &winErr) : install::Unregister(g_exeDir, err, &winErr);
    // this account's HKCU must not hold a copy either (the GUI also cleans the user's own HKCU: the elevated process
    // may run as another account)
    const int cu = install::RemoveHkcuDuplicates();
    if (cu) LOGI("tray: removed %d HKCU duplicate(s) of %s", cu, elog::Narrow(install::JsonName()).c_str());
    if (!ok) {
        LOGE("tray: %s failed: %s", elog::Narrow(arg).c_str(), elog::Narrow(err).c_str());
        CliPrint(L"error: " + err);
        return winErr == ERROR_ACCESS_DENIED ? 5 : 1;
    }
    const std::wstring msg = reg ? L"registered: " + install::OurValueName(g_exeDir) + L" = 0 (HKLM OpenXR implicit layers)"
                                 : std::wstring(L"unregistered: no ") + install::JsonName() + L" value left (HKLM OpenXR implicit layers)";
    LOGI("tray: %s", elog::Narrow(msg).c_str());
    CliPrint(msg);
    return 0;
}

// EyeCaptureTray.exe --inputs [seconds]: lists the Xbox controllers and DirectInput game controllers (wheels, pedals,
// button boxes ...), then for N seconds (default 15) prints every button press as the text the `hotkey` setting takes
// ("Pad:A", "Joy:Thrustmaster T300RS Racing Wheel:Button3"). Hold several buttons for a combination; the text is printed
// when you let go. Read-only: nothing is sent to a game, nothing is saved. Keyboard keys are not listed (use the
// Settings window's "Press a key...").
int RunInputs(const std::wstring& arg) {
    unsigned secs = 15;
    {
        const std::wstring rest = arg.size() > 8 ? arg.substr(8) : std::wstring();
        const unsigned v = (unsigned)wcstoul(rest.c_str(), nullptr, 10);
        if (v > 0 && v <= 3600) secs = v;
    }
    g_exeDir = ExeDir();
    elog::Init(g_exeDir + L"\\logs\\tray.log", false);
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    elog::SetLevel(cfg.logLevel);
    LOGI("tray: --inputs %u s", secs);
    input::Start(nullptr);
    if (!input::WaitReady(10000)) CliPrint(L"(device scan still running after 10 s)");
    auto describe = [](const std::vector<input::DeviceInfo>& devs) {
        if (devs.empty()) CliPrint(L"  none found");
        for (const auto& d : devs) {
            if (d.kind == L"pad")
                CliPrint(L"  Xbox / XInput controller in slot " + std::to_wstring(d.index) + L"  -> bindings Pad:<button> or Pad" +
                         std::to_wstring(d.index) + L":<button>");
            else if (d.xinputSkipped)
                CliPrint(L"  \"" + d.name + L"\"  -> XInput device, skipped here (it is read as an Xbox controller: Pad:<button>)");
            else
                CliPrint(L"  \"" + d.name + L"\"  " + std::to_wstring(d.buttons) + L" buttons" +
                         (d.povs ? L", " + std::to_wstring(d.povs) + L" POV hat(s)" : std::wstring()) +
                         L"  -> bindings Joy:" + d.name + L":Button<n>");
        }
    };
    std::vector<input::DeviceInfo> devs = input::Devices();
    std::wstring devText = input::DevicesText(devs);
    CliPrint(L"Game controllers:");
    describe(devs);
    CliPrint(L"Current hotkey setting: " + cfg.hotkey.text);
    CliPrint(L"Press controller / wheel buttons now (hold several for a combination; the text appears when you let go). "
             L"Listening for " + std::to_wstring(secs) + L" s...");
    input::SetCapture(true);
    const ULONGLONG until = GetTickCount64() + secs * 1000ull;
    while (GetTickCount64() < until) {
        Sleep(30);
        std::wstring t;
        if (input::TakeCaptured(t)) CliPrint(L"  pressed: " + t);
        const std::vector<input::DeviceInfo> now = input::Devices();
        const std::wstring nowText = input::DevicesText(now);
        if (nowText != devText) {
            devText = nowText;
            CliPrint(L"Game controllers changed:");
            describe(now);
        }
    }
    input::SetCapture(false);
    input::Stop();
    CliPrint(L"done. Put a printed text into the Settings window's Hotkey field (or config.ini: hotkey = ...), "
             L"several separated by commas.");
    return 0;
}

// Command-line mode (no tray): EyeCaptureTray.exe --start | --stop | --toggle | --status
// For scripts, Stream Deck buttons, and tests. Prints one status line; exit code 0 = a live game received the command.
// --status also reports the layer registration, registered=yes|no|stale, and autostart=yes|no ("Start with Windows",
// the HKCU Run value) (read-only).
int RunCli(const std::wstring& arg) {
    auto print = [](const std::wstring& s) { CliPrint(s); };
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
    std::wstring text = line;
    if (arg == L"--status") {
        ApplyTestRegRoots();
        ApplyTestRunKey(GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_SHM", nullptr, 0) > 0);
        const install::Status rs = install::Query(ExeDir());
        text += rs.state == install::State::StalePath ? L" registered=stale"
              : rs.state == install::State::NotRegistered ? L" registered=no" : L" registered=yes";
        if (rs.hkcuDuplicate) text += L" hkcu_duplicate=yes";
        if (!rs.filesPresent) text += L" files=missing";
        text += install::AutostartEnabled() ? L" autostart=yes" : L" autostart=no";
    }
    print(text);
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
        if (a == L"--register" || a == L"--unregister") return RunRegCli(a);
        if (a == L"--inputs" || a.compare(0, 9, L"--inputs ") == 0) return RunInputs(a);
    }
    // Test-only: EYECAP_TRAY_TEST_SHM=<name> runs an extra tray instance on a private status block (no single-instance
    // check, no tray-alive marker), so tests/status_sim.exe can drive it without touching a real game's status.
    wchar_t testShm[128];
    const bool testMode = GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_SHM", testShm, 128) > 0;
    g_testMode = testMode;
    if (testMode) settings::SetReporter(SettingsReporter);   // tests: log what the message box would show
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
    ApplyTestRunKey(testMode);   // before the settings window can open
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
        elog::SetLevel(cfg.logLevel);   // log_level = none silences tray.log from here on
        std::wstring how;
        const std::wstring ff = pp::ResolveFfmpeg(cfg, g_exeDir, how);
        if (ff.empty()) LOGW("tray: ffmpeg not found at start (%s)", elog::Narrow(how).c_str());
        else LOGI("tray: ffmpeg = %s (%s)", elog::Narrow(ff).c_str(), elog::Narrow(how).c_str());
    }
    // Layer registration (tray/install.h). Test mode skips it unless EYECAP_TRAY_TEST_REG=1 (tests/run_install.ps1,
    // together with EYECAP_TRAY_TEST_REG_ROOTS so nothing real is read or written).
    {
        wchar_t testReg[8] = {};
        g_regEnabled = !testMode || (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_REG", testReg, 8) > 0 && wcscmp(testReg, L"1") == 0);
        if (g_regEnabled) {
            ApplyTestRegRoots();
            const int removed = install::RemoveHkcuDuplicates();   // never needs admin; the HKLM entry is the one games use
            if (removed) LOGI("tray: removed %d per-user (HKCU) duplicate registration(s) of the layer", removed);
            QueryReg(true);
            PostMessageW(g_wnd, WM_CHECK_REG, 0, 0);
        }
    }
    // "Start with Windows" (HKCU Run value, tray/install.h): keep the entry pointed at this folder; runs in normal mode too
    if (install::SyncAutostart(g_exeDir))
        LOGI("tray: start-with-Windows entry pointed at another folder, updated to %s", elog::Narrow(install::AutostartCommand(g_exeDir)).c_str());
    LOGI("tray: start with Windows: %s", install::AutostartEnabled() ? "on" : "off");
    // controller / wheel hotkeys (Pad: / Joy: bindings): the tray polls them and sends ECC_TOGGLE to the game
    ApplyInputBindings("start");
    input::Start(OnInputFire);
    Refresh();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    input::Stop();   // before the status block is unmapped (the poll thread sends commands through it)
    for (HICON i : g_icons) if (i) DestroyIcon(i);
    if (g_shm) UnmapViewOfFile(g_shm);
    if (g_map) CloseHandle(g_map);
    if (alive) CloseHandle(alive);
    if (single) CloseHandle(single);
    return 0;
}
