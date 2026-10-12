// postprocess -- see postprocess.h.
#include "postprocess.h"
#include <shlobj.h>
#include <cmath>
#include <cwctype>
#include "log.h"

namespace pp {
namespace {

std::wstring FileName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}
std::wstring StripExt(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    const size_t d = p.find_last_of(L'.');
    return (d == std::wstring::npos || (s != std::wstring::npos && d < s)) ? p : p.substr(0, d);
}
std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }
std::string  N(const std::wstring& s) { return elog::Narrow(s); }

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool FileSize(const std::wstring& p, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return true;
}

// First non-empty line of ffmpeg's output ("" if none).
std::wstring FirstLine(const std::string& out) {
    const std::wstring w = elog::Widen(out);
    size_t i = 0;
    while (i < w.size()) {
        size_t e = w.find_first_of(L"\r\n", i);
        if (e == std::wstring::npos) e = w.size();
        size_t a = i, b = e;
        while (a < b && iswspace(w[a])) ++a;
        while (b > a && iswspace(w[b - 1])) --b;
        if (b > a) return w.substr(a, b - a);
        i = e + 1;
    }
    return L"";
}

// Recursive search (depth-limited, no reparse points) for ffmpeg.exe below `dir`.
std::wstring FindFfmpegBelow(const std::wstring& dir, int depth) {
    const std::wstring direct = dir + L"\\ffmpeg.exe";
    if (FileExists(direct)) return direct;
    if (depth <= 0) return L"";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        found = FindFfmpegBelow(dir + L"\\" + fd.cFileName, depth - 1);
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Value of "key":"value" in a flat one-line JSON object ("" if absent). Enough for the recorder's sidecar.
std::string JsonString(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = j.find('"', p + 1);
    if (p == std::string::npos) return "";
    const size_t e = j.find('"', p + 1);
    return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
}

// Value of a "key=value" line in ffprobe's `-of default=nw=1` output ("" if absent).
std::string ProbeValue(const std::string& out, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t p = 0;
    while ((p = out.find(k, p)) != std::string::npos) {
        if (p == 0 || out[p - 1] == '\n' || out[p - 1] == '\r') {
            const size_t s = p + k.size();
            const size_t e = out.find_first_of("\r\n", s);
            return out.substr(s, e == std::string::npos ? std::string::npos : e - s);
        }
        p += k.size();
    }
    return "";
}

// "66/1", "60000/1001" or "30" -> frames per second. False for "0/0" and garbage.
bool ParseRate(const std::string& s, double& fps) {
    double num = 0.0, den = 1.0;
    const size_t slash = s.find('/');
    char* end = nullptr;
    num = strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    if (slash != std::string::npos) {
        const char* d = s.c_str() + slash + 1;
        den = strtod(d, &end);
        if (end == d) return false;
    }
    if (!(num > 0.0) || !(den > 0.0)) return false;
    fps = num / den;
    return true;
}

// The per-eye 3D layout filter graph shared by the MKV and the 8K step (flat side-by-side input only).
// Both eyes use the same expression on the same input size, so their geometry is identical. No padding.
//   lay       the layout (layout_3d)
//   shortSide 0 = NATIVE size (no upscale: sc = min(1, 8192/long side of the frame)); else the FRAME's short side S
//             (sc = min(S/short side, 8192/long side))
//   fpsCap    "" or "fps=60," (goes first, so the scalers only see the kept frames)
// Eye units: the cropped half of the SBS input is iw x ih. The frame is fw x fh in eye units: iw x ih for the half layouts
// (each eye squeezed to half height / width), 2*iw x ih for sbs_full, iw x 2*ih for tb_full. Stored eye size:
// 2*round(iw*sc/(2 or 4)) x 2*round(ih*sc/(2 or 4)), 4 in the squeezed direction (all dimensions even).
struct LayoutGraph {
    std::wstring vf;               // -vf value (without quotes)
    const wchar_t* stereoMode;     // Matroska stereo_mode: left_right / top_bottom
    const char* name;              // layout_3d value
    const char* rule;              // human-readable description for the log
};
// eye_crop: when not None and the eye (iw/2 x ih of the SBS input) is taller than the target aspect (16:9 or 4:3), each eye is
// centre-cropped vertically to that aspect BEFORE scaling: height = 2*trunc(eyeW*D/N/2) (even), y = even centre offset; both eyes use
// the same numbers. An eye that is already wider than the target is left alone (min(ih, ...)).
// EyeAspect gives the target as N:D (0 = no crop).
void EyeAspect(EyeCrop c, int& n, int& d) {
    n = c == EyeCrop::Wide169 ? 16 : c == EyeCrop::Std43 ? 4 : 0;
    d = c == EyeCrop::Wide169 ? 9 : c == EyeCrop::Std43 ? 3 : 1;
}
// The same arithmetic as the filter graph, for the log line: the frame ffmpeg will make from an SBS input inW x inH.
// Returns the cropped eye size (cw x ch) and the frame size (fw x fh).
void PredictFrame(Layout3d lay, EyeCrop crop, int shortSide, int inW, int inH, int& cw, int& ch, int& fw, int& fh) {
    cw = inW / 2;
    ch = inH;
    int n, d;
    EyeAspect(crop, n, d);
    if (n > 0) {
        const int want = 2 * (int)std::floor((double)cw * d / n / 2.0);
        if (want < ch) ch = want;
    }
    const double ew = (double)(lay == Layout3d::SbsFull ? 2 * cw : cw), eh = (double)(lay == Layout3d::TbFull ? 2 * ch : ch);
    const double longCap = 8192.0 / (ew > eh ? ew : eh);
    double sc = longCap;
    if (shortSide > 0) {
        const double s = (double)shortSide / (ew < eh ? ew : eh);
        if (s < sc) sc = s;
    } else if (sc > 1.0) {
        sc = 1.0;
    }
    const double wDiv = lay == Layout3d::SbsHalf ? 4.0 : 2.0, hDiv = lay == Layout3d::TbHalf ? 4.0 : 2.0;
    const int sw = 2 * (int)std::floor(cw * sc / wDiv + 0.5), sh = 2 * (int)std::floor(ch * sc / hDiv + 0.5);
    const bool sbs = lay == Layout3d::SbsHalf || lay == Layout3d::SbsFull;
    fw = sbs ? 2 * sw : sw;
    fh = sbs ? sh : 2 * sh;
}
LayoutGraph BuildLayoutGraph(Layout3d lay, EyeCrop crop, int shortSide, const std::wstring& fpsCap) {
    LayoutGraph g;
    const bool sbs = lay == Layout3d::SbsHalf || lay == Layout3d::SbsFull;
    g.stereoMode = sbs ? L"left_right" : L"top_bottom";
    g.name = lay == Layout3d::SbsHalf ? "sbs_half" : lay == Layout3d::SbsFull ? "sbs_full"
             : lay == Layout3d::TbFull ? "tb_full" : "tb_half";
    g.rule = lay == Layout3d::TbHalf ? "frame = one eye's aspect, each eye squeezed to half height, left eye on top"
             : lay == Layout3d::SbsHalf ? "frame = one eye's aspect, each eye squeezed to half width, left eye left"
             : lay == Layout3d::SbsFull ? "frame = 2x one eye's aspect (side by side), each eye at its own aspect"
                                        : "frame = one eye's aspect / 2 (top-bottom), each eye at its own aspect";
    const std::wstring fw = lay == Layout3d::SbsFull ? L"2*iw" : L"iw";
    const std::wstring fh = lay == Layout3d::TbFull ? L"2*ih" : L"ih";
    const std::wstring longCap = L"8192/max(" + fw + L"\\," + fh + L")";
    const std::wstring sc = shortSide > 0
        ? L"min(" + std::to_wstring(shortSide) + L"/min(" + fw + L"\\," + fh + L")\\," + longCap + L")"
        : L"min(1\\," + longCap + L")";
    const wchar_t* wDiv = lay == Layout3d::SbsHalf ? L"4" : L"2";   // sbs_half: eye width halved
    const wchar_t* hDiv = lay == Layout3d::TbHalf ? L"4" : L"2";    // tb_half: eye height halved
    const std::wstring fit = L"scale=w=2*round(iw*" + sc + L"/" + wDiv + L"):h=2*round(ih*" + sc + L"/" + hDiv + L"):flags=lanczos";
    int cn, cd;
    EyeAspect(crop, cn, cd);
    const std::wstring cropH = cn > 0 ? L"min(ih\\,2*trunc(iw/2*" + std::to_wstring(cd) + L"/" + std::to_wstring(cn) + L"/2))" : L"ih";
    const std::wstring cropY = cn > 0 ? L"trunc((ih-oh)/4)*2" : L"0";
    const std::wstring cl = L"crop=iw/2:" + cropH + L":0:" + cropY, cr = L"crop=iw/2:" + cropH + L":iw/2:" + cropY;
    g.vf = fpsCap + L"split=2[l][r];[l]" + cl + L"," + fit + L"[L];[r]" + cr + L"," + fit +
           std::wstring(L"[R];[L][R]") + (sbs ? L"hstack" : L"vstack") + L"=inputs=2,setsar=1,format=yuv420p";
    return g;
}

// NtSuspendProcess / NtResumeProcess (ntdll, undocumented but stable since XP): suspend / resume every thread of a process.
using NtProcFn = LONG(NTAPI*)(HANDLE);
NtProcFn NtProc(const char* name) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt ? reinterpret_cast<NtProcFn>(reinterpret_cast<void*>(GetProcAddress(nt, name))) : nullptr;
}

} // namespace

Recording ReadSidecar(const std::wstring& mp4) {
    Recording r;
    r.sidecar = StripExt(mp4) + L".json";
    HANDLE h = CreateFileW(r.sidecar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;   // no sidecar: flat SBS (also every recording made before dome mode)
    char buf[1024];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok) return r;
    const std::string j(buf, got);
    const std::string proj = JsonString(j, "projection"), layout = JsonString(j, "layout");
    r.hasSidecar = true;
    if (proj == "vr180") r.projection = Recording::Vr180;
    else if (proj == "vr360") r.projection = Recording::Vr360;
    r.topBottom = layout == "top_bottom" || (layout.empty() && r.projection == Recording::Vr360);
    return r;
}

const wchar_t* Recording::Name() const {
    return projection == Vr180 ? L"VR180" : projection == Vr360 ? L"VR360" : L"3D SBS";
}

std::wstring ResolveFfmpeg(const Config& cfg, const std::wstring& exeDir, std::wstring& how) {
    if (!cfg.ffmpegPath.empty()) {
        if (FileExists(cfg.ffmpegPath)) { how = L"ffmpeg_path from config.ini"; return cfg.ffmpegPath; }
        how = L"ffmpeg_path from config.ini does not exist: " + cfg.ffmpegPath;
        return L"";
    }
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) { how = L"PATH"; return buf; }
    wchar_t la[MAX_PATH];
    const DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (m > 0 && m < MAX_PATH) {
        const std::wstring r = FindFfmpegBelow(std::wstring(la) + L"\\Microsoft\\WinGet\\Packages", 6);
        if (!r.empty()) { how = L"WinGet packages"; return r; }
    }
    const std::wstring local = exeDir + L"\\ffmpeg\\ffmpeg.exe";
    if (FileExists(local)) { how = L"ffmpeg folder next to the tray app"; return local; }
    how = L"not on PATH, not in WinGet packages, not in " + exeDir + L"\\ffmpeg";
    return L"";
}

// ------------------------------------------------------------------------------------------------------------------

void Processor::Start(const std::wstring& exeDir, HWND wnd, UINT msg) {
    exeDir_ = exeDir;
    wnd_ = wnd;
    msg_ = msg;
    thread_ = std::thread([this] { Worker(); });
}

void Processor::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
        if (childSuspended_) SuspendChildLocked(false);   // resume first: never leave a suspended ffmpeg behind
        if (child_) TerminateProcess(child_, 1);   // the job notices quit_, deletes its partial output and returns
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool Processor::Enqueue(const std::wstring& mp4, bool force) {
    std::lock_guard<std::mutex> lk(m_);
    if (quit_) return false;
    if (_wcsicmp(running_.c_str(), mp4.c_str()) == 0) {
        if (force && !runningForced_) {   // "Convert last recording now" on the running (maybe paused) job: never hold it again
            runningForced_ = true;
            if (childSuspended_) SuspendChildLocked(false);
            LOGI("queue: %s is already running -- now forced (ignores the hold)", N(FileName(mp4)).c_str());
            cv_.notify_all();
            return true;
        }
        return false;
    }
    for (auto& q : queue_) {
        if (_wcsicmp(q.path.c_str(), mp4.c_str()) != 0) continue;
        if (force && !q.force) {   // "Convert last recording now" on a held job: it may run now
            q.force = true;
            LOGI("queue: %s was waiting for the hold -- forced, runs now", N(FileName(mp4)).c_str());
            cv_.notify_all();
            return true;
        }
        return false;
    }
    queue_.push_back(Job{mp4, force});
    lastQueued_ = mp4;
    if (hold_ && !force)
        LOGI("queue: %s queued while held -- converts %s", N(FileName(mp4)).c_str(), N(holdReason_).c_str());
    cv_.notify_all();
    return true;
}

bool Processor::StartableLocked() const {
    if (queue_.empty()) return false;
    if (!hold_) return true;
    for (const auto& q : queue_) if (q.force) return true;
    return false;
}

void Processor::SuspendChildLocked(bool suspend) {
    if (!child_ || childSuspended_ == suspend) return;
    static const NtProcFn s_suspend = NtProc("NtSuspendProcess");
    static const NtProcFn s_resume = NtProc("NtResumeProcess");
    const NtProcFn fn = suspend ? s_suspend : s_resume;
    const LONG status = fn ? fn(child_) : (LONG)0xC0000002L;   // STATUS_NOT_IMPLEMENTED
    if (status >= 0) {
        childSuspended_ = suspend;
        LOGI("job: ffmpeg (pid %lu) %s", GetProcessId(child_), suspend ? "SUSPENDED (hold)" : "resumed");
    } else {
        LOGW("job: %s ffmpeg (pid %lu) failed (NTSTATUS 0x%08lX)", suspend ? "suspending" : "resuming", GetProcessId(child_),
             (unsigned long)status);
    }
}

void Processor::SetHold(bool hold, const std::wstring& reason) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        const std::wstring r = hold ? reason : std::wstring();
        changed = hold != hold_ || r != holdReason_;
        if (!changed) return;
        if (hold != hold_)
            LOGI("queue: hold %s%s%s (%zu queued, %s)", hold ? "ON -- conversions wait, converts " : "OFF",
                 hold ? N(r).c_str() : "", hold ? "" : " -- conversions may run", queue_.size(),
                 busy_ ? (runningForced_ ? "a forced job is running" : "a job is running") : "no job running");
        hold_ = hold;
        holdReason_ = r;
        // a running non-forced job: suspend its ffmpeg while held, resume it when released
        if (busy_ && !runningForced_) SuspendChildLocked(hold);
    }
    cv_.notify_all();
    Poke();
}

State Processor::GetState() {
    std::lock_guard<std::mutex> lk(m_);
    State s = st_;
    s.queued = queue_.size();
    s.held = hold_;
    s.holdReason = holdReason_;
    s.paused = PausedLocked();
    return s;
}

bool Processor::PopNotice(Notice& n) {
    std::lock_guard<std::mutex> lk(m_);
    if (notices_.empty()) return false;
    n = notices_.front();
    notices_.pop_front();
    return true;
}

std::wstring Processor::LastQueued() {
    std::lock_guard<std::mutex> lk(m_);
    return lastQueued_;
}

void Processor::Poke() {
    if (wnd_) PostMessageW(wnd_, msg_, 0, 0);
}

void Processor::SetStep(const wchar_t* step) {
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.step = step;
    }
    Poke();
}

void Processor::Finish(bool ok, const std::wstring& lastLine, const Notice* notice) {
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.lastLine = lastLine;
        st_.lastOk = ok;
        if (notice) notices_.push_back(*notice);
    }
    Poke();
}

void Processor::Worker() {
    for (;;) {
        std::wstring job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return quit_ || StartableLocked(); });
            if (quit_) break;
            // not held: the oldest job; held: the oldest FORCED job (the others keep waiting, in order)
            auto it = queue_.begin();
            if (hold_) while (!it->force) ++it;   // StartableLocked guarantees one exists
            job = it->path;
            runningForced_ = it->force;
            queue_.erase(it);
            running_ = job;
            busy_ = true;
            if (runningForced_ && hold_) LOGI("job: %s is forced -- runs although conversions are held", N(FileName(job)).c_str());
            st_.busy = true;
            st_.file = FileName(job);
            st_.step = L"MKV";
        }
        Poke();
        RunJob(job);
        {
            std::lock_guard<std::mutex> lk(m_);
            running_.clear();
            busy_ = false;
            runningForced_ = false;
            st_.busy = false;
            st_.file.clear();
            st_.step.clear();
        }
        Poke();
    }
}

// Runs a command line with stdout+stderr merged and captured, at below-normal priority so a game keeps its CPU.
bool Processor::RunFfmpeg(const std::wstring& cmdline, std::string& output, DWORD& exitCode) {
    output.clear();
    exitCode = (DWORD)-1;
    {
        // held (post_when) and not a forced job: do not start the next ffmpeg until the hold clears (or the tray exits)
        std::unique_lock<std::mutex> lk(m_);
        if (PausedLocked() && !quit_) {
            LOGI("job: held -- the next ffmpeg step waits until the hold clears");
            cv_.wait(lk, [this] { return quit_ || !PausedLocked(); });
            if (!quit_) LOGI("job: hold cleared -- continuing");
        }
        if (quit_) { output = "cancelled (tray exiting)"; return false; }
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    const BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                   nullptr, nullptr, &si, &pi);
    const DWORD createErr = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        char msg[64];
        snprintf(msg, sizeof(msg), "could not start ffmpeg (error %lu)", (unsigned long)createErr);
        output = msg;
        return false;
    }
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(m_);
        child_ = pi.hProcess;
        childSuspended_ = false;
        if (quit_) TerminateProcess(pi.hProcess, 1);
        else if (PausedLocked()) SuspendChildLocked(true);   // the hold came on between the wait above and CreateProcess
    }
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
        if (output.size() < 65536) output.append(buf, got);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    {
        std::lock_guard<std::mutex> lk(m_);
        child_ = nullptr;
        childSuspended_ = false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return true;
}

void Processor::RunJob(const std::wstring& mp4) {
    const ULONGLONG t0 = GetTickCount64();
    const std::wstring name = FileName(mp4);
    auto quitting = [this] { std::lock_guard<std::mutex> lk(m_); return quit_; };
    auto fail = [&](const std::wstring& why, bool balloon = true) {
        LOGE("job: FAILED %s: %s (MP4 left untouched)", N(name).c_str(), N(why).c_str());
        Notice n{L"Conversion failed", name + L"\n" + why, true};
        Finish(false, L"last: " + name + L" conversion FAILED (see tray.log)", balloon ? &n : nullptr);
    };

    Config cfg;
    LoadConfig(exeDir_, cfg);
    LOGI("job: start %s (post_mkv=%d post_flat=%d post_yt8k=%d keep_mp4=%d)", N(name).c_str(), cfg.postMkv, cfg.postFlat,
         cfg.postYt8k, cfg.keepMp4);
    if (!cfg.postMkv && !cfg.postFlat && !cfg.postYt8k) {
        LOGI("job: post_mkv, post_flat and post_yt8k are all 0 -- nothing to do");
        Notice n{L"Nothing to convert", L"post_mkv, post_flat and post_yt8k are all 0 in config.ini.", false};
        Finish(true, L"", &n);
        return;
    }

    std::wstring how;
    const std::wstring ff = ResolveFfmpeg(cfg, exeDir_, how);
    if (ff.empty()) {
        LOGE("job: ffmpeg not found (%s)", N(how).c_str());
        Notice n{L"ffmpeg not found", L"ffmpeg not found — run: winget install Gyan.FFmpeg.Essentials", true};
        Finish(false, L"last: " + name + L" not converted (ffmpeg not found)", &n);
        return;
    }
    LOGI("job: ffmpeg = %s (%s)", N(ff).c_str(), N(how).c_str());

    // The layer has just closed the file; give the filesystem a moment if it is still locked.
    for (int i = 0;; ++i) {
        HANDLE h = CreateFileW(mp4.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); break; }
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) { fail(L"source MP4 no longer exists"); return; }
        if (i >= 40 || quitting()) { fail(L"source MP4 is locked"); return; }
        Sleep(250);
    }
    unsigned long long mp4Size = 0;
    FileSize(mp4, mp4Size);

    const Recording rec = ReadSidecar(mp4);
    LOGI("job: %s -> %s (%s, Matroska stereo_mode=%s)", rec.hasSidecar ? N(FileName(rec.sidecar)).c_str() : "no sidecar",
         N(rec.Name()).c_str(), rec.Dome() ? "projection side data from the MP4's sv3d box is carried over" : "flat side by side",
         rec.topBottom ? "top_bottom" : "left_right");
    const std::wstring base = StripExt(mp4);
    const std::wstring mkv = base + L".mkv";
    const std::wstring flat = base + L"_FLAT.mp4";
    const std::wstring yt8k = base + L"_YT8K.mkv";
    std::string out;
    DWORD code = 0;
    bool mkvOk = false, flatOk = false, yt8kOk = false;

    // hevc_nvenc detection, shared by the flat and the 8K step: asked once per job, only when a re-encode step runs.
    int nvencState = -1;   // -1 = not asked yet
    auto haveNvenc = [&]() {
        if (nvencState < 0) {
            std::string encOut;
            DWORD encCode = 0;
            bool nv = RunFfmpeg(Q(ff) + L" -hide_banner -encoders", encOut, encCode) && encCode == 0 &&
                      encOut.find("hevc_nvenc") != std::string::npos;
            wchar_t noNv[8];
            if (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_NO_NVENC", noNv, 8) > 0) {   // test-only: never touch the GPU
                LOGI("job: EYECAP_TRAY_TEST_NO_NVENC is set -- hevc_nvenc not used (test)");
                nv = false;
            }
            nvencState = nv ? 1 : 0;
        }
        return nvencState == 1;
    };
    // One re-encode: `common + encNv + tail` with hevc_nvenc when available, else (or when it fails) `common + encSw + tail`.
    // `swWarn` (optional) is logged as a warning before the software encoder runs. Afterwards `ran` / `code` / `out`
    // describe the last attempt. Returns false when the tray is exiting (`outFile` deleted, nothing else to do).
    auto encode = [&](const char* step, const std::wstring& common, const std::wstring& encNv, const std::wstring& encSw,
                      const char* swName, const char* swWarn, const std::wstring& tail, const std::wstring& outFile, bool& ran) {
        ran = false;
        code = (DWORD)-1;
        if (haveNvenc()) {
            const std::wstring cmd = common + encNv + tail;
            LOGI("job: %s cmd (hevc_nvenc): %s", step, N(cmd).c_str());
            ran = RunFfmpeg(cmd, out, code);
            if (quitting()) { DeleteFileW(outFile.c_str()); LOGW("job: cancelled (tray exiting)"); return false; }
            if (!ran || code != 0) LOGW("job: hevc_nvenc failed (%s) -- retrying with %s", N(FirstLine(out)).c_str(), swName);
        } else {
            LOGI("job: hevc_nvenc not available -- using %s", swName);
        }
        if (!ran || code != 0) {
            if (swWarn) LOGW("job: %s", swWarn);
            const std::wstring cmd = common + encSw + tail;
            LOGI("job: %s cmd (%s): %s", step, swName, N(cmd).c_str());
            ran = RunFfmpeg(cmd, out, code);
            if (quitting()) { DeleteFileW(outFile.c_str()); LOGW("job: cancelled (tray exiting)"); return false; }
        }
        return true;
    };

    // ffprobe sits next to ffmpeg in every ffmpeg build ("" = not found: durations / frame rate are then not checked).
    std::wstring probe;
    {
        const size_t slash = ff.find_last_of(L"\\/");
        const std::wstring cand = (slash == std::wstring::npos ? std::wstring() : ff.substr(0, slash + 1)) + L"ffprobe.exe";
        if (FileExists(cand)) probe = cand;
    }
    // Container duration in seconds (false when ffprobe is missing or cannot read the file).
    auto probeDuration = [&](const std::wstring& file, double& sec) {
        if (probe.empty()) return false;
        std::string po;
        DWORD pc = 0;
        if (!RunFfmpeg(Q(probe) + L" -v error -show_entries format=duration -of default=nw=1 " + Q(file), po, pc) || pc != 0) return false;
        const std::string v = ProbeValue(po, "duration");
        char* end = nullptr;
        sec = strtod(v.c_str(), &end);
        return !v.empty() && end != v.c_str() && sec > 0.0;
    };

    // Logs the crop and the frame size the graph will produce (needs ffprobe; the graph itself does not depend on it).
    auto logFrame = [&](const char* step, const LayoutGraph& g, int shortSide) {
        int n, d;
        EyeAspect(cfg.eyeCrop, n, d);
        const char* cn = cfg.eyeCrop == EyeCrop::Wide169 ? "16:9" : cfg.eyeCrop == EyeCrop::Std43 ? "4:3" : "none";
        std::string po;
        DWORD pc = 0;
        int iw = 0, ih = 0;
        if (!probe.empty() && RunFfmpeg(Q(probe) + L" -v error -select_streams v:0 -show_entries stream=width,height -of default=nw=1 " + Q(mp4), po, pc) &&
            pc == 0) {
            iw = atoi(ProbeValue(po, "width").c_str());
            ih = atoi(ProbeValue(po, "height").c_str());
        }
        if (iw < 4 || ih < 4) {
            LOGI("job: %s: eye_crop %s (frame size not logged: input size unknown)", step, cn);
            return;
        }
        int cw, ch, fw, fh;
        PredictFrame(cfg.layout3d, cfg.eyeCrop, shortSide, iw, ih, cw, ch, fw, fh);
        const bool cropped = ch < ih;
        LOGI("job: %s: input %dx%d = eyes %dx%d; eye_crop %s -> %s%dx%d; %s frame %dx%d", step, iw, ih, iw / 2, ih, cn,
             cropped ? "centre-cropped to " : "no crop needed, eyes ", cw, ch, g.name, fw, fh);
    };

    // MKV step. layout_3d sbs_full (and every dome recording) = plain remux, nothing re-encoded: stream copy, Matroska
    // stereo_mode tag, verified by size (> 90% of the MP4). Any other layout = re-encode with the same per-eye layout filter
    // graph as the 8K step at NATIVE size (no upscale, long side <= 8192), source frame rate kept, hevc_nvenc (else libx265),
    // audio copied; verified by existence, size > 0 and duration (ffprobe) against the MP4.
    if (cfg.postMkv) {
        SetStep(L"MKV");
        const Layout3d lay = cfg.layout3d;
        const bool reencode = !rec.Dome() && (lay != Layout3d::SbsFull || cfg.eyeCrop != EyeCrop::None);
        const ULONGLONG s0 = GetTickCount64();
        unsigned long long mkvSize = 0;
        bool have = false, ran = false;
        if (!reencode) {
            if (rec.Dome())
                LOGI("job: MKV step: %s recording -- layout_3d does not apply; plain remux (-c copy), stereo_mode=%s", N(rec.Name()).c_str(),
                     rec.topBottom ? "top_bottom" : "left_right");
            else
                LOGI("job: MKV step: layout_3d sbs_full + eye_crop none -- plain remux (-c copy) of the full side-by-side recording, stereo_mode=left_right");
            const std::wstring cmd = Q(ff) + L" -hide_banner -loglevel error -y -i " + Q(mp4) +
                                     L" -map 0 -c copy -metadata:s:v:0 stereo_mode=" + (rec.topBottom ? L"top_bottom " : L"left_right ") +
                                     Q(mkv);
            LOGI("job: MKV cmd: %s", N(cmd).c_str());
            ran = RunFfmpeg(cmd, out, code);
            if (quitting()) { DeleteFileW(mkv.c_str()); LOGW("job: cancelled (tray exiting)"); return; }
        } else {
            const LayoutGraph g = BuildLayoutGraph(lay, cfg.eyeCrop, 0, L"");
            LOGI("job: MKV step: layout_3d %s: %s; native size (no upscale, long side <= 8192), source frame rate kept, re-encode",
                 g.name, g.rule);
            logFrame("MKV step", g, 0);
            const std::wstring common = Q(ff) + L" -hide_banner -loglevel error -y -i " + Q(mp4) + L" -map 0:v:0 -map 0:a? -vf \"" +
                                        g.vf + L"\" ";
            const std::wstring tail = std::wstring(L" -c:a copy -max_muxing_queue_size 4096 -metadata:s:v:0 stereo_mode=") + g.stereoMode +
                                      L" " + Q(mkv);
            const std::wstring encNv = L"-c:v hevc_nvenc -preset p5 -rc vbr -tier high -b:v 150M -maxrate 200M -bufsize 300M";
            const std::wstring encX = L"-c:v libx265 -preset medium -crf 18";
            if (!encode("MKV", common, encNv, encX, "libx265",
                        "MKV step: no working hevc_nvenc -- libx265 at this size is slow (many times the clip length)", tail, mkv, ran))
                return;
        }
        have = FileSize(mkv, mkvSize);
        const double sec = (GetTickCount64() - s0) / 1000.0;
        if (!ran || code != 0) {
            DeleteFileW(mkv.c_str());
            std::wstring why = FirstLine(out);
            if (why.empty()) why = L"ffmpeg exit code " + std::to_wstring(code);
            LOGE("job: MKV step FAILED after %.1f s: %s", sec, N(why).c_str());
            fail(why);
            return;
        }
        if (!reencode) {
            if (!have || mkvSize * 10 <= mp4Size * 9) {
                DeleteFileW(mkv.c_str());
                LOGE("job: MKV step: output %s is %llu bytes vs MP4 %llu bytes (< 90%%)", have ? "too small" : "missing", mkvSize, mp4Size);
                fail(L"the MKV is missing or smaller than 90% of the MP4");
                return;
            }
            LOGI("job: MKV verification (remux): exists, %llu bytes = %.1f%% of the MP4's %llu bytes (> 90%%) -- ok", mkvSize,
                 mp4Size ? 100.0 * (double)mkvSize / (double)mp4Size : 0.0, mp4Size);
        } else {
            if (!have || mkvSize == 0) {
                DeleteFileW(mkv.c_str());
                LOGE("job: MKV step: re-encoded output %s", have ? "is empty" : "is missing");
                fail(L"the re-encoded MKV is missing or empty");
                return;
            }
            if (probe.empty()) {
                LOGW("job: MKV verification (re-encode): exists, %llu bytes > 0; ffprobe not found next to ffmpeg -- duration not checked", mkvSize);
            } else {
                double dMp4 = 0.0, dMkv = 0.0;
                const bool okMp4 = probeDuration(mp4, dMp4), okMkv = probeDuration(mkv, dMkv);
                if (quitting()) { DeleteFileW(mkv.c_str()); LOGW("job: cancelled (tray exiting)"); return; }
                if (!okMp4 || !okMkv) {
                    DeleteFileW(mkv.c_str());
                    LOGE("job: MKV verification FAILED: ffprobe could not read the duration of the %s", !okMkv ? "MKV" : "MP4");
                    fail(L"could not verify the re-encoded MKV (ffprobe could not read its duration)");
                    return;
                }
                const double tol = dMp4 * 0.02 > 1.0 ? dMp4 * 0.02 : 1.0;
                if (fabs(dMp4 - dMkv) > tol) {
                    DeleteFileW(mkv.c_str());
                    LOGE("job: MKV verification FAILED: duration %.3f s vs MP4 %.3f s (allowed difference %.2f s)", dMkv, dMp4, tol);
                    fail(L"the re-encoded MKV's duration differs from the MP4's");
                    return;
                }
                LOGI("job: MKV verification (re-encode): exists, %llu bytes > 0, duration %.3f s vs MP4 %.3f s (within %.2f s) -- ok",
                     mkvSize, dMkv, dMp4, tol);
            }
        }
        mkvOk = true;
        LOGI("job: MKV step ok in %.1f s (%s, %llu bytes, MP4 %llu bytes) -> %s", sec, reencode ? "re-encoded" : "remuxed", mkvSize, mp4Size,
             N(FileName(mkv)).c_str());
    }

    if (cfg.postFlat) {
        SetStep(L"flat");
        // Flat SBS: left half, 16:9 centre crop. Dome: the left eye's view straight ahead as a normal 16:9 picture,
        // 100 x 67.7 degrees (v360; vr180 = left half, half-equirect; vr360 = top half, full equirect).
        std::wstring vf = L"crop=iw/2:ih:0:0,crop=iw:min(ih\\,iw*9/16),scale=3840:-2";
        if (rec.projection == Recording::Vr180)
            vf = L"crop=iw/2:ih:0:0,v360=input=hequirect:output=flat:h_fov=100:v_fov=67.7:w=3840:h=2160";
        else if (rec.projection == Recording::Vr360)
            vf = L"crop=iw:ih/2:0:0,v360=input=equirect:output=flat:h_fov=100:v_fov=67.7:w=3840:h=2160";
        const std::wstring common = Q(ff) + L" -hide_banner -loglevel error -y -i " + Q(mp4) + L" -map 0:v:0 -map 0:a? -vf \"" + vf + L"\" ";
        const std::wstring tail = L" -c:a copy -movflags +faststart " + Q(flat);
        const std::wstring encNv = L"-c:v hevc_nvenc -preset p5 -rc vbr -b:v 40M -maxrate 80M -tag:v hvc1";
        const std::wstring encX = L"-c:v libx264 -preset medium -crf 18";
        const ULONGLONG s0 = GetTickCount64();
        bool ran = false;
        if (!encode("flat", common, encNv, encX, "libx264", nullptr, tail, flat, ran)) return;
        const double sec = (GetTickCount64() - s0) / 1000.0;
        unsigned long long flatSize = 0;
        if (!ran || code != 0 || !FileSize(flat, flatSize) || flatSize == 0) {
            DeleteFileW(flat.c_str());
            std::wstring why = FirstLine(out);
            if (why.empty()) why = L"ffmpeg exit code " + std::to_wstring(code);
            LOGE("job: flat step FAILED after %.1f s: %s", sec, N(why).c_str());
            fail(L"flat version: " + why);   // the MKV (if made) stays, and so does the MP4
            return;
        }
        flatOk = true;
        LOGI("job: flat step ok in %.1f s (%llu bytes) -> %s", sec, flatSize, N(FileName(flat)).c_str());
    }

    // 8K copy for YouTube (layout: layout_3d, default tb_half): YouTube puts a 4640 / 5804 wide SBS frame in its 4K tier (1920 per eye, low bitrate); a frame
    // with a short side of 4320 gets the 8K tier and about 3x the bitrate. YouTube reads a 3D upload as HALF format: it
    // assumes the aspect of the WHOLE frame is the aspect of ONE eye (a full 7680x4320 side-by-side looked 2x stretched
    // wide). So the default is half top-bottom: each eye keeps its own display aspect, is squeezed to half its height and
    // the left eye goes on top; the frame is then exactly one eye's aspect. The other layouts (sbs_half, sbs_full, tb_full)
    // are for players that read them. For all four the FRAME's short side is S (4320) and its long side is capped at 8192
    // (BuildLayoutGraph, shared with the MKV step, which uses the native size instead).
    if (cfg.postYt8k) {
        SetStep(L"8K");
        if (rec.Dome()) {
            LOGI("job: 8K step skipped: %s recordings are already full-frame", N(rec.Name()).c_str());
        } else {
            int shortSide = 4320;
            wchar_t sz[32];
            if (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_YT8K_SHORT", sz, 32) > 0) {   // test-only: small output
                int n = 0;
                wchar_t* end = nullptr;
                n = (int)wcstol(sz, &end, 10);
                if (end && *end == 0 && n >= 16 && n <= 8192 && n % 2 == 0) {
                    shortSide = n;
                    LOGW("job: 8K step: EYECAP_TRAY_TEST_YT8K_SHORT=%s (test) -- short side %d instead of 4320", N(sz).c_str(), n);
                } else {
                    LOGW("job: 8K step: EYECAP_TRAY_TEST_YT8K_SHORT=%s ignored (want an even number 16..8192)", N(sz).c_str());
                }
            }
            // YouTube plays at most 60 fps: cap a faster source (72 / 90 Hz headsets) with the fps filter. Without ffprobe the
            // frame rate is left alone. The cap goes first in the graph: same result as capping at the end, and the scalers
            // then see only the kept frames.
            std::wstring fpsCap;
            if (probe.empty()) {
                LOGW("job: 8K step: ffprobe.exe not found next to ffmpeg -- frame rate not checked (no 60 fps cap)");
            } else {
                const std::wstring pcmd = Q(probe) + L" -v error -select_streams v:0 -show_entries stream=r_frame_rate,avg_frame_rate"
                                                     L" -of default=nw=1 " + Q(mp4);
                double fps = 0.0;
                if (RunFfmpeg(pcmd, out, code) && code == 0 && ParseRate(ProbeValue(out, "r_frame_rate"), fps)) {
                    const std::string r = ProbeValue(out, "r_frame_rate"), a = ProbeValue(out, "avg_frame_rate");
                    if (fps > 60.0) {
                        fpsCap = L"fps=60,";
                        LOGI("job: 8K step: source r_frame_rate %s (%.3f fps, avg %s) > 60 -- capped to 60 fps (YouTube plays at most 60)",
                             r.c_str(), fps, a.c_str());
                    } else {
                        LOGI("job: 8K step: source r_frame_rate %s (%.3f fps, avg %s) <= 60 -- frame rate kept", r.c_str(), fps, a.c_str());
                    }
                } else {
                    if (quitting()) { LOGW("job: cancelled (tray exiting)"); return; }
                    LOGW("job: 8K step: ffprobe could not read the frame rate (%s) -- no 60 fps cap", N(FirstLine(out)).c_str());
                }
            }
            const LayoutGraph g = BuildLayoutGraph(cfg.layout3d, cfg.eyeCrop, shortSide, fpsCap);
            LOGI("job: 8K step: layout_3d %s: %s; frame short side %d, long side <= 8192", g.name, g.rule, shortSide);
            logFrame("8K step", g, shortSide);
            const std::wstring common = Q(ff) + L" -hide_banner -loglevel error -y -i " + Q(mp4) + L" -map 0:v:0 -map 0:a? -vf \"" +
                                        g.vf + L"\" ";
            const std::wstring tail = std::wstring(L" -c:a copy -max_muxing_queue_size 4096 -metadata:s:v:0 stereo_mode=") + g.stereoMode +
                                      L" " + Q(yt8k);
            const std::wstring encNv = L"-c:v hevc_nvenc -preset p5 -rc vbr -tier high -b:v 150M -maxrate 200M -bufsize 300M";
            const std::wstring encX = L"-c:v libx265 -preset medium -crf 18";
            const ULONGLONG s0 = GetTickCount64();
            bool ran = false;
            if (!encode("8K", common, encNv, encX, "libx265",
                        "8K step: no working hevc_nvenc -- libx265 at 8K is slow (many times the clip length)",
                        tail, yt8k, ran))
                return;
            const double sec = (GetTickCount64() - s0) / 1000.0;
            unsigned long long ySize = 0;
            if (!ran || code != 0 || !FileSize(yt8k, ySize) || ySize == 0) {
                DeleteFileW(yt8k.c_str());
                std::wstring why = FirstLine(out);
                if (why.empty()) why = L"ffmpeg exit code " + std::to_wstring(code);
                LOGE("job: 8K step FAILED after %.1f s: %s", sec, N(why).c_str());
                fail(L"8K version: " + why);   // the MKV / flat file (if made) stay, and so does the MP4
                return;
            }
            yt8kOk = true;
            LOGI("job: 8K step ok in %.1f s (%llu bytes) -> %s", sec, ySize, N(FileName(yt8k)).c_str());
        }
    }

    // Delete the MP4 only when the verified MKV replaces it and every requested step worked.
    std::wstring deleted;
    if (mkvOk && !cfg.keepMp4) {
        if (DeleteFileW(mp4.c_str())) {
            LOGI("job: deleted %s (keep_mp4 = 0)", N(name).c_str());
            deleted = L" (MP4 removed)";
            if (rec.hasSidecar) {   // the MKV carries the projection itself; the sidecar only described the MP4
                if (DeleteFileW(rec.sidecar.c_str())) LOGI("job: deleted %s with the MP4", N(FileName(rec.sidecar)).c_str());
                else LOGW("job: could not delete %s (error %lu)", N(FileName(rec.sidecar)).c_str(), GetLastError());
            }
        } else {
            LOGW("job: could not delete %s (error %lu)", N(name).c_str(), GetLastError());
        }
    }

    const std::wstring ready = rec.Dome() ? std::wstring(L"Ready for YouTube (") + rec.Name() + L"): " : std::wstring(L"Ready for YouTube: ");
    std::wstring mainOut, text;
    if (mkvOk) { mainOut = FileName(mkv); text = ready + mainOut; }
    else if (yt8kOk) { mainOut = FileName(yt8k); text = L"Ready for YouTube (8K): " + mainOut; }
    else if (flatOk) { mainOut = FileName(flat); text = L"Flat 2D version ready: " + mainOut; }
    if (mkvOk && yt8kOk) text += L"\nYouTube 8K: " + FileName(yt8k);
    if (flatOk && (mkvOk || yt8kOk)) text += L"\nFlat 2D: " + FileName(flat);
    if (mainOut.empty()) {   // only the 8K step was requested and it was skipped (dome recording): nothing was made
        LOGI("job: done %s in %.1f s -- nothing made (8K step skipped)", N(name).c_str(), (GetTickCount64() - t0) / 1000.0);
        Notice skip{L"Nothing to convert", name + L"\n" + rec.Name() + L" recordings are already full-frame; the 8K copy is for flat 3D recordings.", false};
        Finish(true, L"last: " + name + L" (8K copy skipped: " + rec.Name() + L")", &skip);
        return;
    }
    Notice n{L"OpenXR-EyeCapture", text, false};
    LOGI("job: done %s in %.1f s%s", N(mainOut).c_str(), (GetTickCount64() - t0) / 1000.0, N(deleted).c_str());
    Finish(true, L"last: " + mainOut + L" ready", &n);
}

} // namespace pp
