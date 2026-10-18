// EyeCaptureTray -- tray control for VR EyeCapture: the OpenXR layer and the SteamVR proxy (Win32, no frameworks). The
// layer is dormant (pure pass-through) unless this app is running: it holds the named mutex
// Local\OpenXR-EyeCapture-TrayAlive while it runs.
//
// Tray icon: grey = no game, green = game running (ready), red = recording, amber = starting / finalizing / failed,
// blue = converting a finished recording (post-processing, see tray/postprocess.h).
// Left-click: the app window (all settings + the conversion queue / progress). Right-click menu: Start / Stop
// recording, Open captures folder, Convert last recording now, Convert a recording... (standard Open dialog, one or more MP4s, queued forced like "Convert now",
// each with config.ini's conversion settings as its format snapshot), Open log,
// Open VR EyeCapture (bold: = left-click), Edit config, Register / Unregister for VR games..., SteamVR games... (the
// per-game SteamVR proxy, tray/steamvr_ui.h), Exit.
// While a conversion runs, the menu starts with a greyed info line ("Converting <file> (8K) 42% - about 6 min left"),
// "Pause conversion" / "Resume conversion" (suspends / resumes the ffmpeg; the ETA ignores paused time) and
// "Cancel conversion" (ends the ffmpeg, deletes that step's partial output, keeps the MP4, skips the rest of the job;
// queued jobs continue). See pp::Processor::TogglePause / Cancel. Exit while converting does the same cleanup (Stop).
// Test hooks (the menu uses TrackPopupMenu(TPM_RETURNCMD), so WM_COMMAND cannot drive it): post to the message-only
// window of class "OpenXREyeCaptureTray": WM_APP+3 = open the window, WM_APP+5 = pause / resume toggle, WM_APP+6 = cancel,
// WM_APP+7 = "Convert last recording now", WM_APP+9 = the "SteamVR games" window, WM_APP+11 = "Unregister from VR games..."
// (the menu items call the same code); WM_APP+1 (WM_TRAY) with lParam NIN_BALLOONUSERCLICK = a click on the last balloon.
// The class name and the Local\OpenXR-EyeCapture-* object names keep the old spelling on purpose (docs/RENAME_PLAN.md).
// Test mode (EYECAP_TRAY_TEST_SHM): balloons are logged ("tray: balloon [title] text"), never shown on the desktop; question
// boxes are answered No (or by the file EYECAP_TRAY_TEST_ASK, one answer per line); nothing is ever started elevated.
// SteamVR games (tray/steamvr_ui.h): at start the tray only CHECKS the games the user enabled (read-only); when some need a
// repair (game update, older recorder, VR EyeCapture moved) one balloon says so and a click on it opens the SteamVR games
// window, whose Repair button does it. No game folder is written without that click.
// Registration (tray/install.h): at start the tray removes its own HKCU duplicates (the one registry write at start without
// a click, and only when such a copy exists: it would make the loader load the layer twice) and checks HKLM; when the layer is
// not registered for this folder it offers to register it and runs itself elevated (one UAC prompt) with --register.
// Unregister also offers to put the original openvr_api.dll back in the SteamVR games the user enabled.
// Tooltip: game, API, resolution, fps, frames dropped, elapsed, conversion progress. Talks to the layer through
// src/shared_status.h. After each recording the tray remuxes it to .mkv (YouTube 3D tag) in a worker thread.
// post_when (config.ini, re-read when the file changes): now = convert at once; session_end = hold the conversion queue
// while an OpenXR session is live (the layer releases the status block -- ownerPid 0, ECS_NONE -- when the game destroys
// its last session, even if the process keeps running); game_exit (default) = also hold while the process that made a
// queued recording is still alive; manual = a hold that never clears: recordings are queued and wait until the user starts
// them (the window's "Conversions" -> Start / Start all / Run now, "Convert a recording...", "Convert last recording now").
// "Add to queue..." jobs wait for Start / Start all whatever post_when says (pp::Processor userHeld); so does every
// job restored from queue.json after a tray restart.
// See UpdateHold() and pp::Processor::SetHold. Switching to manual keeps the queue; its jobs simply stay held (a running
// non-forced job is suspended, like any hold) and are listed in the window's queue view.
// Start it BEFORE the game: the layer decides at xrCreateSession whether to arm (config require_tray=0 = always arm).
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>   // LoadIconMetric
#include <shlobj.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "config.h"
#include "glass.h"
#include "input.h"
#include "install.h"
#include "postprocess.h"
#include "settings.h"
#include "shared_status.h"
#include "steamvr_ui.h"
#include "openvr/openvr_hooks.h"   // EYECAP_HOME_REG_KEY / _VALUE, EYECAP_TEST_HOME_KEY_ENV

namespace {
const UINT WM_TRAY = WM_APP + 1;
const UINT WM_PP = WM_APP + 2;     // the post-processing worker changed state / queued a notice
const UINT WM_OPEN_SETTINGS = WM_APP + 3;   // also posted by tests/settings_test.exe to open the app window
const UINT WM_CHECK_REG = WM_APP + 4;       // posted once at start: registration prompt (inside the message loop)
const UINT WM_PAUSE_JOB = WM_APP + 5;       // pause / resume the running conversion (= the menu item; tests post it)
const UINT WM_CANCEL_JOB = WM_APP + 6;      // cancel the running conversion (= the menu item; tests post it)
const UINT WM_CONVERT_LAST = WM_APP + 7;    // "Convert last recording now" (= the menu item; tests post it)
const UINT WM_TAG3D = WM_APP + 8;           // a "Tag a video as 3D" thread has a result to show (balloon)
const UINT WM_OPEN_STEAMVR = WM_APP + 9;    // open the "SteamVR games" window (= the menu item; tests post it)
const UINT WM_STEAMVR_REPAIR = WM_APP + 10; // posted once at start: CHECK the SteamVR games the user enabled (read-only; one
                                            // clickable balloon when some need a repair -- the click opens the window)
const UINT WM_UNREGISTER = WM_APP + 11;     // "Unregister from VR games..." (the app window's Tools button; tests post it)
const UINT WM_REGISTER = WM_APP + 12;       // "Register for VR games..." (the app window's Tools button)
const UINT kTimer = 1;
const UINT kTipTimer = 2;     // one-shot: end of the "settings saved" tooltip
enum : UINT { ID_TOGGLE = 100, ID_FOLDER, ID_CONVERT, ID_LOG, ID_SETTINGS, ID_CONFIG, ID_EXIT, ID_REGISTER, ID_UNREGISTER,
               ID_CONVERT_PICK, ID_JOB_INFO, ID_JOB_PAUSE, ID_JOB_CANCEL, ID_TAG3D_SBS, ID_TAG3D_TB, ID_STEAMVR, ID_REMOVE };
const wchar_t kAppTitle[] = L"VR EyeCapture";
// tray icon frames (resource 101 + value); recording and converting alternate A / B (lens colours swapped) every 0.5 s
enum : int { kIcoNoGame, kIcoReady, kIcoRecA, kIcoRecB, kIcoBusy, kIcoConvA, kIcoConvB, kIcoCount };

HINSTANCE      g_inst;
HWND           g_wnd;
NOTIFYICONDATAW g_nid{};
EyeCapShared*  g_shm = nullptr;
HANDLE         g_map = nullptr;
HICON          g_icons[kIcoCount] = {};   // LoadTrayIcon: no game, ready, recording A / B, busy (amber), converting A / B
int            g_iconIdx = -1;
bool           g_blinkOdd = false;             // the tray icon's A / B frame (flipped by the 0.5 s timer)
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
// SteamVR quick mode (steamvr_mode = quick): EyeCaptureMirror.exe runs while vrserver.exe runs (MirrorTick).
SteamVrMode    g_steamvrMode = SteamVrMode::PerGame;   // cached; reloaded with the input bindings when config.ini changes
bool           g_steamvrDome = false;                  // projection is vr180 / vr360 (quick mode records flat only)
HANDLE         g_mirrorProc = nullptr;
HANDLE         g_mirrorQuit = nullptr;                 // Local\OpenXR-EyeCapture-MirrorQuit (manual reset): set = mirror exits
ULONGLONG      g_mirrorCheck = 0;
// relaunch back-off: 3 failed exits (non-zero code other than kMirrorExitBusy) within a minute stop the relaunching until
// steamvr_mode or the SteamVR state changes (g_mirrorBlockedMode / Vr = the state it was blocked in); one balloon
std::deque<ULONGLONG> g_mirrorFails;
bool           g_mirrorBlocked = false;
SteamVrMode    g_mirrorBlockedMode = SteamVrMode::PerGame;
bool           g_mirrorBlockedVr = false;
// Exit code 3 = another EyeCaptureMirror already runs (its single-instance mutex, src/openvr/mirror_main.cpp): not a failure,
// tried again after kMirrorBusyRetryMs (g_mirrorRetryAt).
const DWORD    kMirrorExitBusy = 3;
// Exit code 4 = SteamVR's own openvr_api.dll was not found (quick mode loads SteamVR's copy; VR EyeCapture ships none): one
// balloon, then blocked at once (no 3-failures count) until steamvr_mode or the SteamVR state changes -- retrying every 2 s
// cannot help until SteamVR is (re)installed / restarted.
const DWORD    kMirrorExitNoSteamVr = 4;
const ULONGLONG kMirrorBusyRetryMs = 10000;
ULONGLONG      g_mirrorRetryAt = 0;
// A balloon that does something when clicked (NIN_BALLOONUSERCLICK). Any later balloon replaces it (SetBalloon).
enum class BalloonAction { None, OpenSteamVr };
BalloonAction  g_balloonAction = BalloonAction::None;
DWORD          g_fpsBalloonPid = 0;                // the game process whose uneven-frame-rate balloon was shown (once per session)
std::wstring   g_savedTip;                         // temporary tooltip after Save in the window
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
pp::JobFormat  g_savedFmt;                         // config.ini's conversion keys (same reload): shown for jobs without a snapshot
struct Proc { DWORD pid = 0; HANDLE h = nullptr; };
Proc           g_lastOwner;                        // the last live owner of the status block (ownerPid is 0 once released)
std::vector<Proc> g_producers;
// "Tag a video as 3D" results (tag thread -> UI thread): balloons are shown on the UI thread (WM_TAG3D).
// Heap objects that are never freed on purpose: the detached tag thread may still run while wWinMain returns and the CRT destroys
// statics. g_tagQuit (set in WM_DESTROY) stops it from posting to the dead window.
std::mutex&    g_tagM = *new std::mutex;
std::deque<pp::Notice>& g_tagNotices = *new std::deque<pp::Notice>;
std::atomic<bool> g_tagQuit{false};

// The tray-area icons: the logo mark with a status dot, one .ico per state (tray/icons, resources 101.. in tray/tray.rc,
// made by scripts/make_logo.py), loaded at the small-icon size of the current DPI.
HICON LoadTrayIcon(int i) {
    HICON h = nullptr;
    if (FAILED(LoadIconMetric(g_inst, MAKEINTRESOURCEW(101 + i), LIM_SMALL, &h))) h = LoadIconW(nullptr, IDI_APPLICATION);
    return h;
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

// ---- balloons. Every balloon goes through SetBalloon. Test mode (EYECAP_TRAY_TEST_SHM): logged only, never shown, so
// test runs do not flood the desktop with pop-ups (tests match the log line).
// Fills the balloon fields of g_nid and adds NIF_INFO; the caller's next NIM_MODIFY shows it. log = false: not logged.
void SetBalloon(const std::wstring& title, const std::wstring& text, bool error, bool log = true) {
    if (log) LOGI("tray: balloon [%s] %s", elog::Narrow(title).c_str(), elog::Narrow(text).c_str());
    g_balloonAction = BalloonAction::None;   // a click on this balloon does nothing (the caller may set an action after this)
    if (g_testMode) return;
    g_nid.uFlags |= NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
}

// A balloon on its own (outside Refresh).
void Balloon(const std::wstring& title, const std::wstring& text, bool error) {
    g_nid.uFlags = 0;
    SetBalloon(title, text, error);
    if (g_nid.uFlags) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// " 42% - about 6 min left" for the running step ("" when the percent is unknown; no ETA while paused).
std::wstring ProgressText(const pp::State& j) {
    if (j.percent < 0) return L"";
    std::wstring s = L" " + std::to_wstring(j.percent) + L"%";
    if (!j.paused && j.etaSec >= 0) s += L" - " + pp::EtaText(j.etaSec);
    return s;
}

void PauseJob(const char* from) {
    bool paused = false;
    if (g_pp.TogglePause(paused)) LOGI("tray: %s conversion (%s)", paused ? "pause" : "resume", from);
    else LOGI("tray: pause / resume (%s): no conversion running", from);
}

void CancelJob(const char* from) {
    if (g_pp.Cancel()) LOGI("tray: cancel conversion (%s)", from);
    else LOGI("tray: cancel (%s): no conversion running", from);
}

// ---- post_when: hold the conversion queue while the game / its VR session is still running
const char* PostWhenName(PostWhen w) {
    return w == PostWhen::Now ? "now" : w == PostWhen::SessionEnd ? "session_end" : w == PostWhen::Manual ? "manual" : "game_exit";
}

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

// Shown after "converts " / "resumes " in the tooltip, the menu and the window's queue view.
const wchar_t* HoldReason() {
    return g_postWhen == PostWhen::SessionEnd ? L"when VR stops" : g_postWhen == PostWhen::Manual ? L"when you say" : L"when the game closes";
}

// Computes the hold from post_when and the live status and hands it to the worker (it logs the on / off transitions).
// Called every tick and right before a recording is queued, so a job can never start in the gap.
void UpdateHold() {
    const bool sessionLive = g_live && g_st.state != ECS_NONE;
    bool hold = false;
    if (g_postWhen == PostWhen::SessionEnd) hold = sessionLive;
    else if (g_postWhen == PostWhen::GameExit) hold = ProducersAlive() || sessionLive;
    else if (g_postWhen == PostWhen::Manual) hold = true;   // only forced jobs ever start
    g_pp.SetHold(hold, HoldReason());
}

// Queue a finished recording for post-processing (MP4 only; a no-op when post_mkv, post_flat and post_yt8k are all 0).
// Automatic requests (not forced, no format, not held by the user) are deduplicated by full path for the whole tray session
// (status message vs fallback scan); they have no format snapshot (config.ini at job start).
// Forced requests ("Convert last recording now", "Convert a recording...", the window's "Start") ignore post_when: they run
// next. post_when = manual: automatic recordings are queued and stay held (see UpdateHold) until one of those forces them.
// fmt = the job's own format snapshot (pp::Processor). userHeld = "Add to queue...": waits for Start / Start all.
void QueueRecording(const std::wstring& mp4, bool force, const pp::JobFormat* fmt = nullptr, bool userHeld = false) {
    const bool automatic = !force && !fmt && !userHeld;
    if (automatic) {
        Config cfg;
        LoadConfig(g_exeDir, cfg);
        if (!cfg.postMkv && !cfg.postFlat && !cfg.postYt8k) {
            LOGI("tray: recording %s saved; post_mkv, post_flat and post_yt8k are all 0 -- not converting", elog::Narrow(mp4).c_str());
            return;
        }
        const std::wstring key = NormPath(mp4);
        if (g_queuedPaths.count(key)) {
            LOGI("tray: %s was already queued -- skipping the duplicate", elog::Narrow(mp4).c_str());
            return;
        }
        g_queuedPaths.insert(key);
        AddProducer();
        UpdateHold();   // before Enqueue: the worker must see the hold before it can pick the job
    } else {
        g_queuedPaths.insert(NormPath(mp4));   // the fallback scan must not queue it a second time
    }
    const std::wstring how = force ? L" (forced: ignores post_when)" : userHeld ? L" (waits for Start)" : L"";
    const std::wstring what = fmt ? L" as " + pp::FormatText(*fmt) : L"";
    if (g_pp.Enqueue(mp4, force, fmt, userHeld))
        LOGI("tray: queued %s for conversion%s%s", elog::Narrow(mp4).c_str(), elog::Narrow(what).c_str(), elog::Narrow(how).c_str());
    else LOGI("tray: %s is already queued / running%s", elog::Narrow(mp4).c_str(), elog::Narrow(what).c_str());
}

// True when the MP4 already has converted siblings. Names the tray makes: <stem>.mkv, <stem>_4x3.mkv / _4x3pad / _full.mkv (eye_crop tag),
// <stem>_FLAT.mp4, <stem>_YT8K[_4x3|_4x3pad|_full].mkv, each possibly with " (2)", " (3)" ... before the extension. So: a file in the
// same folder whose name starts with the stem followed by '.', '_' or ' ' (a different recording has a different timestamp in
// its stem) and ends with .mkv (the MKV step: not "_YT8K...") or, with `others`, _FLAT...mp4 / _YT8K...mkv.
bool HasConverted(const std::wstring& mp4, bool others) {
    const size_t slash = mp4.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"." : mp4.substr(0, slash);
    const std::wstring stem = mp4.substr(slash == std::wstring::npos ? 0 : slash + 1, std::wstring::npos);
    const std::wstring base = stem.size() > 4 ? stem.substr(0, stem.size() - 4) : stem;   // without ".mp4"
    auto endsWith = [](const std::wstring& n, const wchar_t* tail) {
        const size_t k = wcslen(tail);
        return n.size() >= k && _wcsicmp(n.c_str() + n.size() - k, tail) == 0;
    };
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring n = fd.cFileName;
        if (n.size() <= base.size() || _wcsnicmp(n.c_str(), base.c_str(), base.size()) != 0) continue;
        const wchar_t c = n[base.size()];
        if (c != L'.' && c != L'_' && c != L' ') continue;
        const bool yt8k = _wcsnicmp(n.c_str() + base.size(), L"_YT8K", 5) == 0;
        const bool flat = _wcsnicmp(n.c_str() + base.size(), L"_FLAT", 5) == 0;
        if (endsWith(n, L".mkv") && !yt8k) found = true;
        else if (others && ((yt8k && endsWith(n, L".mkv")) || (flat && endsWith(n, L".mp4")))) found = true;
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
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
    if (dir.empty()) dir = DefaultCapturesDir(g_exeDir);
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
        if (HasConverted(path, !cfg.postMkv)) continue;   // .mkv (any eye_crop tag / " (2)"), or _FLAT / _YT8K when the MKV step is off
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
            HasConverted(p.path, false)) {
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

    const bool fpsUneven = g_live && pp::UnevenFor60((double)g_st.fps);
    {
        static int s_prevUneven = -1;
        if (s_prevUneven != (int)fpsUneven) {
            s_prevUneven = (int)fpsUneven;
            if (fpsUneven) LOGI("tray: game frame rate %u fps is uneven for 60 fps YouTube video", g_st.fps);
        }
    }
    const pp::State job = g_pp.GetState();
    int idx = 0;
    if (state == ECS_READY) idx = 1;
    else if (state == ECS_RECORDING) idx = 2;
    else if (state == ECS_STARTING || state == ECS_STOPPING || state == ECS_FAILED) idx = 3;
    const bool waiting = job.queued > 0 && (job.held || job.userHeld > 0);   // queued, held by post_when or by the user
    if (job.busy && !job.paused && idx != 2 && idx != 3) idx = 4;   // converting (red recording / amber transitions keep priority)
    else if (idx == 0 && (job.busy || waiting)) idx = 4;            // paused / waiting while no game is live: still blue

    // not registered: the first tooltip line says so (the icon is grey anyway: no game can load the layer from here)
    const bool notReg = g_regEnabled && !RegOk();
    wchar_t tip[256];
    if (!g_live && notReg) {
        swprintf_s(tip, L"VR EyeCapture: no VR game running");   // "(games started after ...)" would be untrue
    } else if (!g_live) {
        swprintf_s(tip, L"VR EyeCapture: no VR game running\n(games started after this app are recorded)");
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
    // with progress: "converting <file> (8K) 42% - about 6 min left"; the log gets the line only when it changes other than
    // by the percent / ETA (no log line per second)
    std::wstring jobLine, jobKey;
    const std::wstring prog = ProgressText(job);
    if (job.busy && job.cancelling) {
        jobLine = L"cancelling: " + job.file + L" (" + job.step + L")";
    } else if (job.busy && job.userPaused) {
        jobLine = L"paused: " + job.file + L" (" + job.step + L")" + prog + L" -- Resume in the tray menu";
        jobKey = L"paused: " + job.file + L" (" + job.step + L")";
        if (job.queued) { jobLine += L" (+" + std::to_wstring(job.queued) + L")"; jobKey += L" +q"; }
    } else if (job.busy && job.paused) {
        jobLine = L"conversion paused: " + job.file + L" -- resumes " + job.holdReason;
        if (job.queued) jobLine += L" (+" + std::to_wstring(job.queued) + L" waiting)";
    } else if (job.busy) {
        jobLine = L"converting " + job.file + L" (" + job.step + L")" + (prog.empty() ? L"..." : prog);
        jobKey = L"converting " + job.file + L" (" + job.step + L")" + (prog.empty() ? L"" : L" [progress]");
        if (job.queued) {
            const std::wstring q = L" +" + std::to_wstring(job.queued) + (job.held ? L" waiting" : L" queued");
            jobLine += q;
            jobKey += q;
        }
    } else if (waiting && job.userHeld == job.queued) {
        jobLine = std::to_wstring(job.queued) + (job.queued == 1 ? L" recording waits" : L" recordings wait") + L" for you (click Start in the window)";
    } else if (waiting) {
        jobLine = std::to_wstring(job.queued) + (job.queued == 1 ? L" recording waiting" : L" recordings waiting") +
                  L" -- converts " + (job.held ? job.holdReason : std::wstring(L"when you start it in the window"));
    } else if (!job.lastLine.empty()) {
        jobLine = job.lastLine;
    }
    if (jobKey.empty()) jobKey = jobLine;
    static std::wstring s_prevJobKey;
    if (jobKey != s_prevJobKey) { s_prevJobKey = jobKey; LOGI("tray: tooltip job line: %s", elog::Narrow(jobLine).c_str()); }
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
    // uneven game frame rate: a short tooltip line (dropped first when the 127 characters are full)
    if (fpsUneven) {
        const std::wstring w = pp::FpsWarningText((double)g_st.fps, true);
        if (wcslen(full) + 1 + w.size() <= 127) {
            const std::wstring withFps = std::wstring(full) + L"\n" + w;
            wcsncpy_s(full, withFps.c_str(), _TRUNCATE);
        }
    }
    static int s_prevNotReg = -1;
    const bool logRegTip = s_prevNotReg != (int)notReg && g_regEnabled;
    s_prevNotReg = (int)notReg;
    if (!g_savedTip.empty() && GetTickCount64() < g_savedTipUntil) wcsncpy_s(full, g_savedTip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP | NIF_ICON;
    wcsncpy_s(g_nid.szTip, full, _TRUNCATE);     // szTip holds 127 characters
    if (logRegTip) LOGI("tray: tooltip now: %s", elog::Narrow(g_nid.szTip).c_str());
    // idx is the state; the icon frame alternates while recording / converting (the timer refreshes every 0.5 s)
    const bool odd = g_blinkOdd;
    const bool converting = job.busy && !job.paused && !job.userPaused;
    const int frame = idx == 0 ? kIcoNoGame : idx == 1 ? kIcoReady : idx == 2 ? (odd ? kIcoRecB : kIcoRecA)
                    : idx == 3 ? kIcoBusy : (converting && odd ? kIcoConvB : kIcoConvA);
    g_nid.hIcon = g_icons[frame];
    // balloon: a conversion result wins over "Recording saved"
    pp::Notice notice;
    if (g_pp.PopNotice(notice)) {
        SetBalloon(notice.title, notice.text, notice.error);
    } else if (g_live && g_prevState == ECS_STOPPING && state == ECS_READY && g_st.message[0]) {
        // a low-space stop says so in the message ("saved <file> -- stopped: E: almost full (1.9 GB free)"): logged + error icon.
        // The message field is 160 characters and the reason follows the full path, so with a long output folder it can be cut:
        // " -- stopped" alone also counts, and a cut text (not ending in "free)") gets a fixed balloon text instead.
        const bool lowDisk = wcsstr(g_st.message, L" almost full") != nullptr || wcsstr(g_st.message, L" -- stopped") != nullptr;
        const size_t ml = wcslen(g_st.message);
        const bool cut = lowDisk && (ml < 5 || wcscmp(g_st.message + ml - 5, L"free)") != 0);
        SetBalloon(lowDisk ? L"Recording stopped: disk almost full" : L"Recording saved",
                   cut ? std::wstring(L"Recording stopped: the output drive is almost full.") : std::wstring(g_st.message), lowDisk, lowDisk);
    } else if (g_live && state == ECS_FAILED && g_prevState != ECS_FAILED && g_st.message[0]) {
        SetBalloon(L"Recording not started", g_st.message, true);   // e.g. the low-space guard (src/disk.h)
    } else if (g_live && state == ECS_RECORDING && fpsUneven && g_fpsBalloonPid != g_st.ownerPid) {
        // once per game session (not per recording): the 60 fps YouTube video of an uneven frame rate stutters
        g_fpsBalloonPid = g_st.ownerPid;
        SetBalloon(L"Frame rate " + std::to_wstring(g_st.fps) + L" fps",
                   L"YouTube plays at most 60 fps, so the video of this recording will stutter a little. Set the headset (Virtual "
                   L"Desktop) to 120 Hz, or 60 Hz, for smooth video.", false);
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
    g_savedFmt = pp::FormatFromConfig(cfg);
    if (cfg.steamvr.mode != g_steamvrMode) LOGI("tray: steamvr_mode = %s", SteamVrModeKey(cfg.steamvr.mode));
    g_steamvrMode = cfg.steamvr.enabled ? cfg.steamvr.mode : SteamVrMode::Off;
    g_steamvrDome = cfg.dome.projection != DomeProjection::Flat;
    static bool s_postWhenLogged = false;
    if (!s_postWhenLogged || cfg.postWhen != g_postWhen) {
        s_postWhenLogged = true;
        g_postWhen = cfg.postWhen;
        LOGI("tray: post_when = %s (%s)", PostWhenName(g_postWhen), g_postWhen == PostWhen::Now ? "convert recordings right away"
             : g_postWhen == PostWhen::SessionEnd ? "convert when no VR session is live"
             : g_postWhen == PostWhen::Manual ? "never convert on its own; recordings wait until started from the window / the tray menu"
             : "convert when the game that made the recording has closed");
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

// Save in the window: log the changed keys, show "settings saved" in the tooltip for 5 s.
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

// ---- SteamVR quick mode: EyeCaptureMirror.exe (src/openvr/mirror_main.cpp) while steamvr_mode = quick and SteamVR runs.
// The process watched is vrserver.exe; a test-mode tray watches only the process named by EYECAP_TRAY_TEST_VRSERVER (no
// mirror at all without it). The mirror's own environment (XR_EYE_CAPTURE_*) is inherited.
bool ProcessRunning(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, exeName) == 0;
    CloseHandle(snap);
    return found;
}

std::wstring MirrorExe() {
    for (const wchar_t* rel : {L"\\EyeCaptureMirror.exe", L"\\build\\Release\\EyeCaptureMirror.exe"}) {
        const std::wstring p = g_exeDir + rel;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return std::wstring();
}

void StopMirror(const char* why) {
    if (!g_mirrorProc) return;
    if (WaitForSingleObject(g_mirrorProc, 0) != WAIT_OBJECT_0) {
        LOGI("tray: SteamVR quick mode: stopping EyeCaptureMirror (%s)", why);
        if (g_mirrorQuit) SetEvent(g_mirrorQuit);   // it finalizes a running recording and exits on its own
    }
    CloseHandle(g_mirrorProc);
    g_mirrorProc = nullptr;
}

// Every 2 s from the timer. Cheap unless quick mode is on or a mirror process is tracked: no process snapshot otherwise.
void MirrorTick() {
    if (g_mirrorProc && WaitForSingleObject(g_mirrorProc, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(g_mirrorProc, &code);
        CloseHandle(g_mirrorProc);
        g_mirrorProc = nullptr;
        if (code == kMirrorExitBusy) {   // another EyeCaptureMirror runs: not a failure, try again later
            g_mirrorRetryAt = GetTickCount64() + kMirrorBusyRetryMs;
            LOGI("tray: SteamVR quick mode: EyeCaptureMirror exited (code 3: another EyeCaptureMirror already runs) -- tried again in %llu s",
                 (unsigned long long)(kMirrorBusyRetryMs / 1000));
        } else {
            LOGI("tray: SteamVR quick mode: EyeCaptureMirror exited (code %lu)", code);
        }
        if (code == kMirrorExitNoSteamVr) {
            g_mirrorBlocked = true;   // no 3-failures count: retrying cannot help until SteamVR is installed / restarted
            g_mirrorBlockedMode = g_steamvrMode;
            g_mirrorBlockedVr = true;
            g_mirrorFails.clear();
            LOGW("tray: SteamVR quick mode: EyeCaptureMirror found no SteamVR openvr_api.dll (code 4) -- not started again until "
                 "steamvr_mode or SteamVR changes (see logs\\eye_capture.log)");
            Balloon(L"SteamVR quick mode", L"Quick mode needs SteamVR installed (openvr_api.dll not found in the SteamVR folder).", true);
        } else if (code != 0 && code != kMirrorExitBusy) {
            const ULONGLONG now = GetTickCount64();
            g_mirrorFails.push_back(now);
            while (!g_mirrorFails.empty() && now - g_mirrorFails.front() > 60000) g_mirrorFails.pop_front();
            if (g_mirrorFails.size() >= 3 && !g_mirrorBlocked) {
                g_mirrorBlocked = true;
                g_mirrorBlockedMode = g_steamvrMode;
                g_mirrorBlockedVr = true;
                LOGW("tray: SteamVR quick mode: EyeCaptureMirror failed 3 times within a minute -- not started again until "
                     "steamvr_mode or SteamVR changes (see logs\\eye_capture.log)");
                Balloon(L"SteamVR quick mode", L"The SteamVR mirror recorder keeps failing, so it is not started again for now. "
                                               L"See logs\\eye_capture.log.", true);
            }
        }
    }
    if (g_steamvrMode != SteamVrMode::Quick && !g_mirrorProc) {   // nothing to start or stop: no process snapshot
        if (g_mirrorBlocked && g_steamvrMode != g_mirrorBlockedMode) {
            g_mirrorBlocked = false;
            g_mirrorFails.clear();
            LOGI("tray: SteamVR quick mode: steamvr_mode changed -- EyeCaptureMirror may start again");
        }
        return;
    }
    wchar_t watch[64] = L"vrserver.exe";
    if (g_testMode && GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_VRSERVER", watch, 64) == 0) return;   // tests: never the real one
    const bool vr = ProcessRunning(watch);
    if (g_mirrorBlocked && (g_steamvrMode != g_mirrorBlockedMode || vr != g_mirrorBlockedVr)) {
        g_mirrorBlocked = false;
        g_mirrorFails.clear();
        LOGI("tray: SteamVR quick mode: %s changed -- EyeCaptureMirror may start again",
             g_steamvrMode != g_mirrorBlockedMode ? "steamvr_mode" : "the SteamVR state");
    }
    const bool want = g_steamvrMode == SteamVrMode::Quick && vr && !g_mirrorBlocked;
    if (!want) {
        StopMirror(g_steamvrMode != SteamVrMode::Quick ? "steamvr_mode is not quick" : !vr ? "SteamVR is not running" : "back-off after failures");
        return;
    }
    if (g_mirrorProc || GetTickCount64() < g_mirrorRetryAt) return;
    const std::wstring exe = MirrorExe();
    if (exe.empty()) {
        static bool logged = false;
        if (!logged) { logged = true; LOGW("tray: SteamVR quick mode: EyeCaptureMirror.exe not found next to the tray app"); }
        return;
    }
    if (!g_mirrorQuit) g_mirrorQuit = CreateEventW(nullptr, TRUE, FALSE, L"Local\\OpenXR-EyeCapture-MirrorQuit");
    if (g_mirrorQuit) ResetEvent(g_mirrorQuit);
    std::wstring cmd = L"\"" + exe + L"\" --home \"" + g_exeDir + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si, &pi)) {
        LOGE("tray: SteamVR quick mode: cannot start %s (error %lu)", elog::Narrow(exe).c_str(), GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    g_mirrorProc = pi.hProcess;
    LOGI("tray: SteamVR quick mode: %s runs -- started %s (pid %lu)", elog::Narrow(watch).c_str(), elog::Narrow(exe).c_str(), pi.dwProcessId);
    if (g_steamvrDome)
        Balloon(L"SteamVR quick mode", L"Quick mode records flat side-by-side only: VR180 / VR360 needs per-game recording (tray menu -> SteamVR games...).", false);
}

void OpenSettings() {
    if (settings::DialogWindow()) { settings::ShowDialog(L"", nullptr); return; }
    settings::ShowDialog(SettingsPath(), OnSettingsSaved);
}

// "SteamVR games..." (tray menu, WM_APP+9): the per-game SteamVR proxy list (tray/steamvr_ui.h).
void OpenSteamVrGames() {
    steamvr_ui::Options o;
    o.installDir = g_exeDir;
    o.testMode = g_testMode;
    o.configPath = SettingsPath();
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    if (!cfg.hotkey.bindings.empty()) o.hotkey = cfg.hotkey.text;
    steamvr_ui::ShowDialog(o);
}

// ---- registration UI ----
// Test mode: the next scripted answer from the file named by EYECAP_TRAY_TEST_ASK (one "Yes" / "No" per line, consumed:
// the first line is removed). "" = no file / no line left (= No).
std::wstring NextTestAnswer() {
    wchar_t path[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_ASK", path, MAX_PATH) == 0) return std::wstring();
    std::string text;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    char b[1024];
    DWORD got = 0;
    while (ReadFile(h, b, sizeof(b), &got, nullptr) && got) text.append(b, got);
    CloseHandle(h);
    std::string first, rest;
    size_t pos = 0;
    while (pos < text.size() && first.empty()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        first = text.substr(pos, e - pos);
        while (!first.empty() && (first.back() == '\r' || first.back() == ' ')) first.pop_back();
        pos = e + 1;
    }
    if (pos < text.size()) rest = text.substr(pos);
    h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        if (!rest.empty()) WriteFile(h, rest.data(), (DWORD)rest.size(), &w, nullptr);
        CloseHandle(h);
    }
    return elog::Widen(first);
}

// Yes / No question. Test mode (EYECAP_TRAY_TEST_SHM): logged and answered No (or the next EYECAP_TRAY_TEST_ASK answer), so
// tests never block on a dialog; a test-mode tray never elevates either (Elevate), so a scripted Yes never reaches UAC.
bool Ask(const std::wstring& text) {
    if (g_testMode) {
        const bool yes = _wcsicmp(NextTestAnswer().c_str(), L"Yes") == 0;
        LOGI("tray: question box: %s -> %s (test mode)", elog::Narrow(text).c_str(), yes ? "Yes" : "No");
        return yes;
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
    if (g_testMode) {   // tests never get a UAC prompt: treated as "declined"
        LOGI("tray: test mode: %s would run elevated now -- not started (treated as UAC declined)", elog::Narrow(arg).c_str());
        launchErr = ERROR_CANCELLED;
        return -1;
    }
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
        q = L"VR EyeCapture is registered for OpenXR games from another folder:\n" + g_reg.registeredPath +
            L"\n\nRe-register it from this folder? Windows will ask for administrator rights once.";
    } else {
        q = L"VR EyeCapture is not registered for OpenXR games yet.\n\nRegister VR EyeCapture for OpenXR games now? Windows "
            L"will ask for administrator rights once.";
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
    const std::wstring q = L"Unregister VR EyeCapture from OpenXR games?\n\nOpenXR games will no longer load the recorder. Your "
                           L"files and recordings stay. Windows will ask for administrator rights once.";
    if (!Ask(q)) { g_regBusy = false; return; }
    // The SteamVR games the user enabled are part of uninstalling too: one more question (per-user files, no admin).
    std::wstring svrText;
    bool svrOk = true;
    const std::vector<std::wstring> svrGames = steamvr_ui::EnabledGameNames(g_exeDir);
    if (!svrGames.empty()) {
        std::wstring list;
        for (const std::wstring& n : svrGames) list += L"  " + n + L"\n";
        if (Ask(L"Also put the original openvr_api.dll back in " + std::to_wstring(svrGames.size()) + L" SteamVR game(s)?\n\n" + list +
                L"\nThose games are then no longer recorded through SteamVR (SteamVR games... can enable them again). Close them and "
                L"SteamVR first."))
            svrText = steamvr_ui::DisableAllGames(g_exeDir, g_testMode, "unregister", &svrOk);
        else
            LOGI("tray: unregister: %zu SteamVR game(s) left enabled", svrGames.size());
    }
    auto withSvr = [&](const std::wstring& t) { return svrText.empty() || !svrOk ? t : t + L" " + svrText; };
    if (!svrOk) ShowError(svrText);   // some game folders could not be changed: say which (the balloons below are short)
    if (!g_regEnabled) {   // test-mode tray without EYECAP_TRAY_TEST_REG: the registry is not touched at all
        LOGI("tray: unregister: test mode without EYECAP_TRAY_TEST_REG -- the OpenXR registration part is skipped");
        if (!svrText.empty() && svrOk) Balloon(L"SteamVR games", svrText, false);
        g_regBusy = false;
        return;
    }
    DWORD launchErr = 0;
    const int code = Elevate(L"--unregister", launchErr);
    install::RemoveHkcuDuplicates();
    QueryReg(true);
    if (code < 0 && launchErr == ERROR_CANCELLED) {
        Balloon(kAppTitle, withSvr(L"Unregistering cancelled: OpenXR games still load VR EyeCapture."), false);
    } else if (code < 0) {
        ShowError(L"Could not start EyeCaptureTray.exe with administrator rights (error " + std::to_wstring(launchErr) +
                  L").\n\nSee logs\\tray.log next to EyeCaptureTray.exe for details.");
    } else if (code != 0) {
        ShowError(ElevatedFailText(L"Unregistering", code));
    } else {
        Balloon(L"Unregistered", withSvr(L"Unregistered. OpenXR games no longer load VR EyeCapture. Exit the tray app or "
                                         L"register again from its menu."), false);
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

// The captures folder: the live game's, else config.ini's output_dir.
std::wstring CapturesDir() {
    std::wstring dir = g_live ? g_st.outputDir : L"";
    if (dir.empty()) {
        Config cfg;
        LoadConfig(g_exeDir, cfg);
        dir = cfg.outputDir;
    }
    return dir;
}

// "Convert last recording now" (tray menu, WM_APP+7): the most recent recording -- the last one this tray queued (also a
// held one: forcing upgrades it), else the newest *.mp4 in the captures folder -- queued forced.
void ConvertLast(const char* from) {
    std::wstring file = g_pp.LastQueued();
    if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring dir = CapturesDir();
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
        LOGI("tray: convert now (%s): no MP4 found to convert", from);
        Balloon(L"Nothing to convert", L"No MP4 found. (Converted recordings have their MP4 removed unless keep_mp4 = 1.)", false);
    } else {
        LOGI("tray: convert now requested for %s (%s)", elog::Narrow(file).c_str(), from);
        QueueRecording(file, true);
    }
    Refresh();
}

// The MP4s the user picks: the standard multi-select Open dialog in the captures folder (`title`). Test mode
// (EYECAP_TRAY_TEST_SHM) with EYECAP_TRAY_TEST_PICK=<file>: no dialog, the picked paths are that file's lines (UTF-8, read at
// every pick, so a test can change them between clicks).
// video = "Tag a video as 3D": *.mkv / *.mp4 / *.mov, starting in the user's Videos folder.
std::vector<std::wstring> PickMp4s(HWND owner, const char* from, const wchar_t* title, bool video = false) {
    std::vector<std::wstring> files;
    wchar_t pickFile[MAX_PATH] = L"";
    if (g_testMode && GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_PICK", pickFile, MAX_PATH) > 0) {
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
            std::string ln = text.substr(pos, e - pos);
            pos = e + 1;
            while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
            if (!ln.empty()) files.push_back(elog::Widen(ln));
        }
        LOGI("tray: %s (%s): EYECAP_TRAY_TEST_PICK -> %zu file(s), no dialog (test)", elog::Narrow(title).c_str(), from, files.size());
        return files;
    }
    std::wstring dir;
    if (video) {
        PWSTR vp = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vp)) && vp) dir = vp;
        CoTaskMemFree(vp);
    }
    if (dir.empty()) dir = CapturesDir();
    std::vector<wchar_t> buf(64 * 1024, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = video ? L"Video files (*.mkv;*.mp4;*.mov)\0*.mkv;*.mp4;*.mov\0"
                            : L"MP4 recordings (*.mp4)\0*.mp4\0";   // recordings only: PickAndQueue refuses anything else
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        const DWORD err = CommDlgExtendedError();
        if (err) LOGW("tray: %s (%s): Open dialog failed (error 0x%lX)", elog::Narrow(title).c_str(), from, (unsigned long)err);
        return files;
    }
    // One file: the buffer holds its full path. Several: the folder, then each file name, then an empty string.
    const std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) files.push_back(first);
    // a drive root comes back as "E:\" (with its separator): no second backslash, so Enqueue's path dedupe still matches
    const std::wstring folder = !first.empty() && (first.back() == L'\\' || first.back() == L'/') ? first : first + L"\\";
    for (; *p; p += wcslen(p) + 1) files.push_back(folder + p);
    return files;
}

// "Convert a recording..." (tray menu, and the window's button) / "Add to queue..." (the window): one or more
// MP4s, each queued with the format snapshot `fmt` (tray menu: config.ini's current values; window: the values the window
// shows). start = forced (runs next, ignores post_when); else held by the user until Start / Start all.
void PickAndQueue(HWND owner, const char* from, const pp::JobFormat& fmt, bool start) {
    const std::vector<std::wstring> files = PickMp4s(owner, from, start ? L"Convert a recording" : L"Add recordings to the queue");
    auto endsWith = [](const std::wstring& n, const wchar_t* tail) {
        const size_t k = wcslen(tail);
        return n.size() >= k && _wcsicmp(n.c_str() + n.size() - k, tail) == 0;
    };
    std::wstring rejected;   // file names of the refused picks, one balloon for all of them
    for (const auto& f : files) {
        // Only recordings: a conversion treats its input as an SBS / dome MP4 and, with keep_mp4 = 0, deletes it afterwards,
        // so a converted .mkv / _FLAT.mp4 (or any other file) picked here would be destroyed.
        if (!endsWith(f, L".mp4") || endsWith(f, L"_FLAT.mp4")) {
            const size_t sl = f.find_last_of(L"\\/");
            LOGW("tray: %s (%s): %s is not a recording (only .mp4 recordings, not _FLAT.mp4) -- not queued",
                 start ? "convert a recording" : "add to queue", from, elog::Narrow(f).c_str());
            rejected += (rejected.empty() ? L"" : L", ") + (sl == std::wstring::npos ? f : f.substr(sl + 1));
            continue;
        }
        LOGI("tray: %s (%s): picked %s", start ? "convert a recording" : "add to queue", from, elog::Narrow(f).c_str());
        QueueRecording(f, start, &fmt, !start);
    }
    if (!rejected.empty()) Balloon(kAppTitle, L"Not a recording: " + rejected, true);
    Refresh();
}

// Tag menu: "Tag a video as 3D" -> "Side by side..." / "Top / bottom...". The picked videos are remuxed one after the other on a
// background thread (pp::Tag3d: -c copy, <name>_3D.mkv with the Matroska stereo_mode tag); it neither uses nor disturbs the
// conversion queue. Each result becomes a balloon (UI thread, WM_TAG3D). A thread still running when the tray exits is
// killed with the process (its ffmpeg is in a kill-on-close job object; a half-written .part may stay).
void TagAsStereo(HWND owner, const char* from, bool topBottom) {
    const wchar_t* title = topBottom ? L"Tag a video as 3D (top / bottom)" : L"Tag a video as 3D (side by side)";
    const std::vector<std::wstring> files = PickMp4s(owner, from, title, true);
    if (files.empty()) return;
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    const std::wstring exeDir = g_exeDir;
    const HWND wnd = g_wnd;
    LOGI("tray: tag as 3D (%s): %zu file(s), %s", from, files.size(), topBottom ? "top_bottom" : "left_right");
    std::thread([files, cfg, exeDir, topBottom, wnd] {
        for (const auto& f : files) {
            std::wstring out, why;
            pp::Notice n;
            if (pp::Tag3d(cfg, exeDir, f, topBottom, out, why)) {
                n.title = kAppTitle;
                n.text = L"Tagged as 3D: " + out.substr(out.find_last_of(L"\\/") + 1);
            } else {
                const size_t sl = f.find_last_of(L"\\/");
                n.title = L"Tag as 3D failed";
                n.text = (sl == std::wstring::npos ? f : f.substr(sl + 1)) + L": " + why;
                n.error = true;
            }
            if (g_tagQuit) return;   // the tray is exiting: nobody shows the balloon
            {
                std::lock_guard<std::mutex> lk(g_tagM);
                g_tagNotices.push_back(n);
            }
            PostMessageW(wnd, WM_TAG3D, 0, 0);
        }
    }).detach();
}

// The window's "Conversions" group and its "Convert a recording..." / "Add to queue..." buttons (tray/settings.h QueueHooks).
void InstallQueueHooks() {
    settings::QueueHooks h;
    h.state = [] { return g_pp.GetState(); };
    h.queued = [] {
        std::vector<pp::QueuedJob> v = g_pp.Queued();
        for (auto& j : v) if (!j.ownFormat) j.fmt = g_savedFmt;   // what it would convert with if it started now
        return v;
    };
    h.pickFiles = [](HWND owner, const pp::JobFormat& fmt, bool start) { PickAndQueue(owner, "settings", fmt, start); };
    h.togglePause = [] { PauseJob("settings"); Refresh(); };
    h.cancel = [] { CancelJob("settings"); Refresh(); };
    h.start = [](unsigned long long id) {
        if (!g_pp.StartJob(id)) LOGI("tray: start (settings): job %llu is no longer waiting", id);
        else LOGI("tray: start (settings): job %llu", id);
        Refresh();
    };
    h.startAll = [] {
        LOGI("tray: start all (settings): %zu job(s) forced", g_pp.StartAll());
        Refresh();
    };
    h.remove = [](unsigned long long id) {
        if (!g_pp.Remove(id)) LOGI("tray: remove (settings): job %llu is no longer waiting", id);
        Refresh();
    };
    h.gameFps = [] { return g_live ? g_st.fps : 0u; };
    settings::SetQueueHooks(h);
    settings::ToolHooks t;   // the app window's "Tools:" row (these were tray menu items)
    t.openLog = [] { OpenPath(g_live && g_st.logPath[0] ? std::wstring(g_st.logPath) : LogsDir(g_exeDir) + L"\\eye_capture.log"); };
    t.tag3d = [](HWND owner, bool tb) { TagAsStereo(owner, "app window", tb); };
    t.regState = [] { return !g_regEnabled ? 0 : RegOk() ? 2 : 1; };
    t.toggleReg = [] { PostMessageW(g_wnd, RegOk() ? WM_UNREGISTER : WM_REGISTER, 0, 0); };   // posted: it may ask / show UAC
    settings::SetToolHooks(t);
    settings::SetOpenSteamVrHook([] { PostMessageW(g_wnd, WM_OPEN_STEAMVR, 0, 0); });   // posted (the window runs its own message loop, which must not nest in the Settings button handler); the Settings window's "SteamVR games..." button = the tray menu item
}

void ShowMenu() {
    if (g_regBusy) return;   // a registration question / UAC prompt is open
    if (g_regEnabled) QueryReg(false);
    Refresh();
    HMENU m = CreatePopupMenu();
    {
        // a conversion runs: greyed info line + Pause / Resume + Cancel at the top
        const pp::State job = g_pp.GetState();
        if (job.busy) {
            std::wstring info;
            if (job.cancelling) info = L"Cancelling: " + job.file + L" (" + job.step + L")";
            else if (job.userPaused) info = L"Paused: " + job.file + L" (" + job.step + L")" + ProgressText(job);
            else if (job.paused) info = L"Waiting: " + job.file + L" (" + job.step + L")" + ProgressText(job) + L" -- converts " + job.holdReason;
            else info = L"Converting " + job.file + L" (" + job.step + L")" + ProgressText(job);
            if (job.queued) info += L" (+" + std::to_wstring(job.queued) + L" queued)";
            std::wstring esc;   // '&' would be a menu mnemonic
            for (wchar_t ch : info) { esc += ch; if (ch == L'&') esc += L'&'; }
            AppendMenuW(m, MF_STRING | MF_GRAYED, ID_JOB_INFO, esc.c_str());
            const UINT can = job.cancelling ? MF_GRAYED : 0;
            AppendMenuW(m, MF_STRING | can, ID_JOB_PAUSE, job.userPaused ? L"Resume conversion" : L"Pause conversion");
            AppendMenuW(m, MF_STRING | can, ID_JOB_CANCEL, L"Cancel conversion");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        }
    }
    const uint32_t state = g_live ? g_st.state : ECS_NONE;
    const bool rec = state == ECS_RECORDING || state == ECS_STARTING;
    std::wstring label = rec ? L"Stop recording" : L"Start recording";
    if (g_live && g_st.hotkey[0]) label += std::wstring(L"\t") + g_st.hotkey;
    AppendMenuW(m, MF_STRING | ((g_live && state != ECS_STOPPING) ? 0 : MF_GRAYED), ID_TOGGLE, label.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // short: the rest (Convert a recording..., Tag as 3D, Open log, config, Register) is in the app window
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Open VR EyeCapture");
    SetMenuDefaultItem(m, ID_SETTINGS, FALSE);   // bold: left-click on the icon does the same
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Open captures folder");
    AppendMenuW(m, MF_STRING, ID_CONVERT, L"Convert last recording now");
    AppendMenuW(m, MF_STRING, ID_STEAMVR, L"SteamVR games...");
    // installed copy: the uninstaller (the same as Windows Settings > Apps > VR EyeCapture > Uninstall)
    if (IsInstalledCopy(g_exeDir) && GetFileAttributesW((g_exeDir + L"\\unins000.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        AppendMenuW(m, MF_STRING, ID_REMOVE, L"Remove VR EyeCapture from this PC...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    glass::Reload();       // Windows may have switched light / dark since the last menu
    glass::ThemeMenus();
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
    case ID_CONVERT:
        ConvertLast("menu");
        break;
    case ID_CONVERT_PICK:
        LoadConfig(g_exeDir, cfg);
        PickAndQueue(g_wnd, "menu", pp::FormatFromConfig(cfg), true);   // config.ini's current conversion settings
        break;
    case ID_TAG3D_SBS:
    case ID_TAG3D_TB:
        TagAsStereo(g_wnd, "menu", cmd == ID_TAG3D_TB);
        break;
    case ID_JOB_PAUSE:
        PauseJob("menu");
        Refresh();
        break;
    case ID_JOB_CANCEL:
        CancelJob("menu");
        Refresh();
        break;
    case ID_LOG:
        OpenPath(g_live && g_st.logPath[0] ? std::wstring(g_st.logPath) : LogsDir(g_exeDir) + L"\\eye_capture.log");
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
    case ID_STEAMVR:
        OpenSteamVrGames();
        break;
    case ID_REMOVE: {   // the uninstaller asks itself, closes this tray (--quit) and cleans up (--uninstall-cleanup)
        const std::wstring un = g_exeDir + L"\\unins000.exe";
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", un.c_str(), nullptr, g_exeDir.c_str(), SW_SHOWNORMAL);
        LOGI("tray: Remove VR EyeCapture from this PC: started %s (%s)", elog::Narrow(un).c_str(), (INT_PTR)r > 32 ? "ok" : "failed");
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
        if (wp == kTipTimer) { KillTimer(h, kTipTimer); g_savedTip.clear(); Refresh(); LOGI("tray: tooltip restored: %s", elog::Narrow(g_nid.szTip).c_str()); return 0; }
        if (g_regEnabled && !g_regBusy && GetTickCount64() - g_regQueried >= 10000) QueryReg(false);   // cheap registry read
        CheckConfigChanged();   // one GetFileAttributesEx: controller / wheel hotkeys follow config.ini edits
        if (GetTickCount64() - g_mirrorCheck >= 2000) { g_mirrorCheck = GetTickCount64(); MirrorTick(); }   // SteamVR quick mode
        g_blinkOdd = !g_blinkOdd;
        Refresh();
        return 0;
    case WM_CHECK_REG:
        StartupRegCheck();
        return 0;
    case WM_PP:
        Refresh();
        settings::NotifyQueueChanged();   // the window's queue view (no-op when it is closed)
        return 0;
    case WM_TAG3D: {
        pp::Notice n;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(g_tagM);
                if (g_tagNotices.empty()) break;
                n = g_tagNotices.front();
                g_tagNotices.pop_front();
            }
            Balloon(n.title, n.text, n.error);
        }
        return 0;
    }
    case WM_OPEN_SETTINGS:
        OpenSettings();
        return 0;
    case WM_OPEN_STEAMVR:   // = menu "SteamVR games..." (tests post it)
        OpenSteamVrGames();
        return 0;
    case WM_STEAMVR_REPAIR: {   // once at start: DETECT games updated / verified since the last run -- nothing is written
        const steamvr_ui::RepairCheck rc = steamvr_ui::CheckRepairs(g_exeDir, "tray start");
        if (rc.games) {
            // the names are in the log (CheckRepairs) and in the window the click opens: the balloon stays short (256 chars max)
            Balloon(L"SteamVR games", L"SteamVR recording: " + std::to_wstring(rc.games) + L" game(s) need a repair after an update. "
                                          L"Click to repair.", false);
            g_balloonAction = BalloonAction::OpenSteamVr;   // NIN_BALLOONUSERCLICK opens the SteamVR games window (Repair)
        }
        return 0;
    }
    case WM_UNREGISTER:   // the app window's "Unregister from VR games..." (tests post it)
        UnregisterFlow();
        return 0;
    case WM_REGISTER:     // the app window's "Register for VR games..."
        if (!g_regBusy) RegisterFlow(false);
        return 0;
    case WM_PAUSE_JOB:   // = menu "Pause / Resume conversion" (tests post it)
        PauseJob("WM_APP+5");
        Refresh();
        return 0;
    case WM_CANCEL_JOB:  // = menu "Cancel conversion" (tests post it)
        CancelJob("WM_APP+6");
        Refresh();
        return 0;
    case WM_CONVERT_LAST:   // = menu "Convert last recording now" (tests post it)
        ConvertLast("WM_APP+7");
        return 0;
    case WM_TRAY:
        // left click: the app window (conversion queue / progress, all settings); right click: the menu
        if (LOWORD(lp) == WM_LBUTTONUP) { if (!g_regBusy) OpenSettings(); }
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        else if (LOWORD(lp) == NIN_BALLOONUSERCLICK) {   // the user clicked the balloon (Windows 10 / 11: the notification)
            const BalloonAction a = g_balloonAction;
            g_balloonAction = BalloonAction::None;
            LOGI("tray: balloon clicked%s", a == BalloonAction::OpenSteamVr ? " -- opening the SteamVR games window" : "");
            if (a == BalloonAction::OpenSteamVr) OpenSteamVrGames();
        }   // (not cleared on NIN_BALLOONTIMEOUT: Windows keeps the notification in the action center, where it can still be clicked)
        return 0;
    case WM_DESTROY:
        StopMirror("tray exit");
        g_tagQuit = true;
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
    elog::Init(LogsDir(g_exeDir) + L"\\tray.log", false);
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

// EyeCaptureTray.exe --quit: closes every running tray of THIS folder (WM_CLOSE to its message window = the menu's Exit) and
// waits up to 10 s for each. For the installer (before files are replaced or removed). Exit code 0 = none left running.
int RunQuit() {
    const std::wstring dir = ExeDir();
    std::vector<HANDLE> procs;
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, L"OpenXREyeCaptureTray", nullptr)) != nullptr;) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!p) continue;
        wchar_t path[MAX_PATH] = L"";
        DWORD n = MAX_PATH;
        std::wstring d;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) { d = path; d = d.substr(0, d.find_last_of(L'\\')); }
        if (_wcsicmp(d.c_str(), dir.c_str()) != 0) { CloseHandle(p); continue; }
        PostMessageW(w, WM_CLOSE, 0, 0);
        procs.push_back(p);
    }
    int left = 0;
    for (HANDLE p : procs) {
        if (WaitForSingleObject(p, 10000) != WAIT_OBJECT_0) ++left;
        CloseHandle(p);
    }
    CliPrint(L"quit: " + std::to_wstring(procs.size()) + L" tray(s) of this folder asked to exit, " + std::to_wstring(left) + L" still running");
    return left ? 1 : 0;
}

// EyeCaptureTray.exe --autostart on|off: the installer's "Start VR EyeCapture with Windows" task (run as the signed-in user,
// not elevated). The same as ticking / clearing "Start with Windows" in the app window and clicking Save: start_with_windows
// in the user's config.ini (seeded from the template first on an installed copy) and the HKCU Run value. A user choice, so
// it may write the Run value (never StartupApproved). Exit code 0 = done, 1 = failed (printed / logged), 2 = usage.
int RunAutostartCli(const std::wstring& a) {
    const std::wstring v = a.size() > 12 ? a.substr(12) : L"";
    if (v != L"on" && v != L"off") { CliPrint(L"usage: EyeCaptureTray.exe --autostart on|off"); return 2; }
    const bool on = v == L"on";
    g_exeDir = ExeDir();
    elog::Init(LogsDir(g_exeDir) + L"\\tray.log", false);
    ApplyTestRunKey(GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_SHM", nullptr, 0) > 0);
    SeedAppDataConfig(g_exeDir);
    settings::SaveResult r;
    if (!settings::SaveKeyValuesToFile(ConfigPath(g_exeDir), {{L"start_with_windows", on ? L"1" : L"0"}}, r)) {
        std::wstring m;
        for (const auto& p : r.problems) m += p + L" ";
        CliPrint(L"error: config.ini: " + m);
        LOGW("tray: --autostart %s: config.ini not written: %s", on ? "on" : "off", elog::Narrow(m).c_str());
        return 1;
    }
    std::wstring err;
    if (!install::SetAutostart(on, g_exeDir, err)) {
        CliPrint(L"error: " + err);
        LOGW("tray: --autostart %s: %s", on ? "on" : "off", elog::Narrow(err).c_str());
        return 1;
    }
    LOGI("tray: --autostart %s: start_with_windows = %d, Run value %s", on ? "on" : "off", on ? 1 : 0, on ? "written" : "deleted");
    CliPrint(on ? L"start with Windows: on" : L"start with Windows: off");
    return 0;
}

// EyeCaptureTray.exe --uninstall-cleanup: what the uninstaller runs (elevated, the user's account) before it deletes the
// files: puts the original openvr_api.dll back in every SteamVR game the user enabled, deletes the HKCU Home value, the
// "Start with Windows" Run value(s) and the HKLM OpenXR layer registration (+ HKCU duplicates). Never asks (the uninstaller
// did). Test keys apply as for the GUI (EYECAP_TRAY_TEST_REG_ROOTS, EYECAP_TRAY_TEST_RUN_KEY, XR_EYE_CAPTURE_TEST_HOME_KEY;
// EYECAP_TRAY_TEST_SHM = test mode). Exit code 0 = everything done, 1 = something failed (logged / printed).
int RunUninstallCleanup() {
    g_exeDir = ExeDir();
    elog::Init(LogsDir(g_exeDir) + L"\\tray.log", false);
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    elog::SetLevel(cfg.logLevel);
    const bool testMode = GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_SHM", nullptr, 0) > 0;
    ApplyTestRegRoots();
    ApplyTestRunKey(testMode);
    LOGI("tray: --uninstall-cleanup (exe folder %s%s)", elog::Narrow(g_exeDir).c_str(), testMode ? ", test mode" : "");
    bool ok = true;
    if (!steamvr_ui::EnabledGameNames(g_exeDir).empty()) {
        bool svrOk = true;
        const std::wstring t = steamvr_ui::DisableAllGames(g_exeDir, testMode, "uninstall", &svrOk);
        CliPrint(L"steamvr: " + t);
        if (!svrOk) ok = false;
    } else {
        CliPrint(L"steamvr: no game enabled");
    }
    steamvr_ui::RemoveHomeKey(g_exeDir, testMode);
    std::wstring err;
    if (install::SetAutostart(false, g_exeDir, err)) CliPrint(L"start with Windows: removed");
    else { ok = false; CliPrint(L"start with Windows: " + err); LOGW("tray: uninstall: %s", elog::Narrow(err).c_str()); }
    long winErr = 0;
    if (testMode && GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_REG_ROOTS", nullptr, 0) == 0)
        CliPrint(L"OpenXR layer: test mode without EYECAP_TRAY_TEST_REG_ROOTS -- skipped");
    else if (install::Unregister(g_exeDir, err, &winErr)) CliPrint(L"OpenXR layer: unregistered");
    else { ok = false; CliPrint(L"OpenXR layer: " + err); LOGW("tray: uninstall: %s", elog::Narrow(err).c_str()); }
    install::RemoveHkcuDuplicates();
    LOGI("tray: --uninstall-cleanup %s", ok ? "done" : "done with errors");
    return ok ? 0 : 1;
}

// EyeCaptureTray.exe --inputs [seconds]: lists the Xbox controllers and DirectInput game controllers (wheels, pedals,
// button boxes ...), then for N seconds (default 15) prints every button press as the text the `hotkey` setting takes
// ("Pad:A", "Joy:Thrustmaster T300RS Racing Wheel:Button3"). Hold several buttons for a combination; the text is printed
// when you let go. Read-only: nothing is sent to a game, nothing is saved. Keyboard keys are not listed (use the
// window's "Press a key...").
int RunInputs(const std::wstring& arg) {
    unsigned secs = 15;
    {
        const std::wstring rest = arg.size() > 8 ? arg.substr(8) : std::wstring();
        const unsigned v = (unsigned)wcstoul(rest.c_str(), nullptr, 10);
        if (v > 0 && v <= 3600) secs = v;
    }
    g_exeDir = ExeDir();
    elog::Init(LogsDir(g_exeDir) + L"\\tray.log", false);
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
    CliPrint(L"done. Put a printed text into the window's Hotkey field (or config.ini: hotkey = ...), "
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
        if (!EyeCapSendCommand(shm, cmd)) { print(L"no VR game with a capture-capable session is running"); rc = 1; }
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

// EyeCaptureTray.exe --tag3d <sbs|tb> <file>: the menu's "Tag a video as 3D" for one file, synchronously (tests, scripts).
// Prints "ok <output file>" / "error: <reason>"; exit code 0 = tagged, 2 = bad arguments, 1 = failed. Logs to logs\tray.log.
int RunTag3d(const std::wstring& arg) {
    g_exeDir = ExeDir();
    elog::Init(LogsDir(g_exeDir) + L"\\tray.log", false);
    std::wstring rest = arg.substr(8);   // after "--tag3d "
    while (!rest.empty() && rest.front() == L' ') rest.erase(0, 1);
    const size_t sp = rest.find(L' ');
    const std::wstring mode = rest.substr(0, sp);
    std::wstring file = sp == std::wstring::npos ? std::wstring() : rest.substr(sp + 1);
    while (!file.empty() && file.front() == L' ') file.erase(0, 1);
    if (file.size() >= 2 && file.front() == L'"' && file.back() == L'"') file = file.substr(1, file.size() - 2);
    if ((mode != L"sbs" && mode != L"tb") || file.empty()) {
        CliPrint(L"usage: EyeCaptureTray.exe --tag3d <sbs|tb> <file>");
        return 2;
    }
    Config cfg;
    LoadConfig(g_exeDir, cfg);
    elog::SetLevel(cfg.logLevel);
    std::wstring out, why;
    if (!pp::Tag3d(cfg, g_exeDir, file, mode == L"tb", out, why)) {
        CliPrint(L"error: " + why);
        return 1;
    }
    CliPrint(L"ok " + out);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && cmdLine[0] == L'-') {
        std::wstring a = cmdLine;
        while (!a.empty() && a.back() == L' ') a.pop_back();
        if (a.compare(0, 8, L"--tag3d ") == 0) return RunTag3d(a);
        if (a == L"--start" || a == L"--stop" || a == L"--toggle" || a == L"--status") return RunCli(a);
        if (a == L"--register" || a == L"--unregister") return RunRegCli(a);
        if (a == L"--quit") return RunQuit();
        if (a == L"--uninstall-cleanup") return RunUninstallCleanup();
        if (a.compare(0, 11, L"--autostart") == 0) return RunAutostartCli(a);
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
    elog::Init(LogsDir(g_exeDir) + L"\\tray.log", false);
    LOGI("tray: started%s (exe folder %s)", testMode ? " [TEST MODE, private status block]" : "", elog::Narrow(g_exeDir).c_str());
    ApplyTestRunKey(testMode);   // before the settings window can open
    // %APPDATA%\OpenXR-EyeCapture\config.ini is copied once to %APPDATA%\VR EyeCapture (a plain file copy, no registry).
    // Nothing in the registry is written at start: no Run / StartupApproved / Home writes (docs/DEVELOPMENT.md "Antivirus").
    {
        std::wstring from, to;
        const int m = MigrateAppDataConfig(&from, &to);
        if (m > 0) LOGI("tray: config.ini copied from the old folder %s to %s (the old file is kept)", elog::Narrow(from).c_str(), elog::Narrow(to).c_str());
        else if (m < 0) LOGW("tray: could not copy %s to %s (error %d); the old file is still used", elog::Narrow(from).c_str(), elog::Narrow(to).c_str(), -m);
        // installed copy (Program Files is read-only): the config the user edits is %APPDATA%\VR EyeCapture\config.ini, seeded
        // once from the commented template in the program folder
        const int s = SeedAppDataConfig(g_exeDir);
        if (s > 0) LOGI("tray: installed copy -- config.ini copied from the program folder to %s", elog::Narrow(ConfigPath(g_exeDir)).c_str());
        else if (s < 0) LOGW("tray: installed copy -- could not create %%APPDATA%%\\VR EyeCapture\\config.ini (error %d)", -s);
        if (IsInstalledCopy(g_exeDir))
            LOGI("tray: installed copy -- logs and lists in %s, recordings by default in %s", elog::Narrow(UserDataDir(g_exeDir)).c_str(),
                 elog::Narrow(DefaultCapturesDir(g_exeDir)).c_str());
    }
    g_shm = testMode ? EyeCapOpenShared(&g_map, testShm) : EyeCapOpenShared(&g_map);
    for (int i = 0; i < kIcoCount; ++i) g_icons[i] = LoadTrayIcon(i);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"OpenXREyeCaptureTray";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(wc.lpszClassName, kAppTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);   // found by class

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icons[0];
    wcsncpy_s(g_nid.szTip, kAppTitle, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    SetTimer(g_wnd, kTimer, 500, nullptr);
    g_pp.Start(g_exeDir, g_wnd, WM_PP);
    PostMessageW(g_wnd, WM_STEAMVR_REPAIR, 0, 0);   // SteamVR games the user enabled: CHECKED (read-only); a balloon offers the repair
    {
        // resolve ffmpeg once at start so the log says what a job would use (jobs resolve again when they run)
        Config cfg;
        LoadConfig(g_exeDir, cfg);
        elog::SetLevel(cfg.logLevel);   // log_level = none silences tray.log from here on
        glass::Init();                  // the tray menu and windows: the saved theme (system / light / dark)
        if (cfg.uiTheme) glass::ForceDark(cfg.uiTheme == 2 ? 1 : 0);   // system: follow Windows (or EYECAP_TEST_GLASS in tests)
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
            // The ONE registry write at start without a click (docs/DEVELOPMENT.md "Antivirus: behaviour rules"): a per-user
            // (HKCU) copy of our OpenXR layer entry is deleted, and only when one exists -- with both HKLM and HKCU the
            // official loader loads the layer twice and the game crashes. Nothing is ever ADDED here; no HKCU copy = no write.
            const int removed = install::RemoveHkcuDuplicates();   // never needs admin; the HKLM entry is the one games use
            if (removed) LOGI("tray: removed %d per-user (HKCU) duplicate registration(s) of the layer", removed);
            QueryReg(true);
            PostMessageW(g_wnd, WM_CHECK_REG, 0, 0);
        }
    }
    // "Start with Windows" (HKCU Run value, tray/install.h). With start_with_windows = 1 in config.ini (the user's choice) the
    // tray puts ITS OWN Run value back when it is missing or points at another folder -- the one start-up registry write
    // (docs/DEVELOPMENT.md "Antivirus: behaviour rules"): only when wrong, never StartupApproved, never over a value the user
    // switched off in Task Manager. Without the key (configs from before 0.5.0) or with 0: READ ONLY, a value that points at
    // another folder is only reported (log + one balloon).
    {
        Config ac;
        std::vector<std::wstring> acp;
        ParseConfigFile(ConfigPath(g_exeDir), ac, acp);
        install::AutostartInfo ai = install::ReadAutostart();
        if (ac.startWithWindowsSet && ac.startWithWindows && !ai.disabledInTaskManager &&
            (!(ai.runPresent || ai.oldPresent) || !install::AutostartPointsHere(ai, g_exeDir))) {
            const std::wstring was = ai.runPresent ? ai.runCommand : ai.oldPresent ? ai.oldCommand : L"(missing)";
            std::wstring err;
            if (install::SetAutostart(true, g_exeDir, err))
                LOGI("tray: start with Windows: start_with_windows = 1 and the Run value was %s -- restored to %s", elog::Narrow(was).c_str(),
                     elog::Narrow(install::AutostartCommand(g_exeDir)).c_str());
            else
                LOGW("tray: start with Windows: could not restore the Run value: %s", elog::Narrow(err).c_str());
            ai = install::ReadAutostart();
        }
        LOGI("tray: start with Windows: %s%s", (ai.runPresent || ai.oldPresent) && !ai.disabledInTaskManager ? "on" : "off",
             ai.disabledInTaskManager ? " (disabled in Task Manager > Startup apps)" : "");
        const std::wstring stale = install::AutostartStalePath(g_exeDir);
        if (!stale.empty()) {
            LOGW("tray: start with Windows points at %s, not at this folder (%s); left unchanged", elog::Narrow(stale).c_str(), elog::Narrow(g_exeDir).c_str());
            Balloon(kAppTitle, L"Start with Windows still points to " + stale + L". Open Settings and tick Start with Windows again to fix it.", false);
        }
    }
    // controller / wheel hotkeys (Pad: / Joy: bindings): the tray polls them and sends ECC_TOGGLE to the game
    ApplyInputBindings("start");
    InstallQueueHooks();   // before the window can open
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
