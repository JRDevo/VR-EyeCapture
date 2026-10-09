// postprocess -- see postprocess.h.
#include "postprocess.h"
#include <shlobj.h>
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

} // namespace

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
        if (child_) TerminateProcess(child_, 1);   // the job notices quit_, deletes its partial output and returns
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool Processor::Enqueue(const std::wstring& mp4) {
    std::lock_guard<std::mutex> lk(m_);
    if (quit_) return false;
    if (_wcsicmp(running_.c_str(), mp4.c_str()) == 0) return false;
    for (const auto& q : queue_) if (_wcsicmp(q.c_str(), mp4.c_str()) == 0) return false;
    queue_.push_back(mp4);
    lastQueued_ = mp4;
    cv_.notify_one();
    return true;
}

State Processor::GetState() {
    std::lock_guard<std::mutex> lk(m_);
    State s = st_;
    s.queued = queue_.size();
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
            cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            job = queue_.front();
            queue_.pop_front();
            running_ = job;
            st_.busy = true;
            st_.file = FileName(job);
            st_.step = L"MKV";
        }
        Poke();
        RunJob(job);
        {
            std::lock_guard<std::mutex> lk(m_);
            running_.clear();
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
        if (quit_) TerminateProcess(pi.hProcess, 1);
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
    LOGI("job: start %s (post_mkv=%d post_flat=%d keep_mp4=%d)", N(name).c_str(), cfg.postMkv, cfg.postFlat, cfg.keepMp4);
    if (!cfg.postMkv && !cfg.postFlat) {
        LOGI("job: post_mkv and post_flat are both 0 -- nothing to do");
        Notice n{L"Nothing to convert", L"post_mkv and post_flat are both 0 in config.ini.", false};
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

    const std::wstring base = StripExt(mp4);
    const std::wstring mkv = base + L".mkv";
    const std::wstring flat = base + L"_FLAT.mp4";
    std::string out;
    DWORD code = 0;
    bool mkvOk = false, flatOk = false;

    if (cfg.postMkv) {
        SetStep(L"MKV");
        const std::wstring cmd = Q(ff) + L" -hide_banner -loglevel error -y -i " + Q(mp4) +
                                 L" -map 0 -c copy -metadata:s:v:0 stereo_mode=left_right " + Q(mkv);
        LOGI("job: MKV cmd: %s", N(cmd).c_str());
        const ULONGLONG s0 = GetTickCount64();
        const bool ran = RunFfmpeg(cmd, out, code);
        if (quitting()) { DeleteFileW(mkv.c_str()); LOGW("job: cancelled (tray exiting)"); return; }
        unsigned long long mkvSize = 0;
        const bool have = FileSize(mkv, mkvSize);
        const double sec = (GetTickCount64() - s0) / 1000.0;
        if (!ran || code != 0) {
            DeleteFileW(mkv.c_str());
            std::wstring why = FirstLine(out);
            if (why.empty()) why = L"ffmpeg exit code " + std::to_wstring(code);
            LOGE("job: MKV step FAILED after %.1f s: %s", sec, N(why).c_str());
            fail(why);
            return;
        }
        if (!have || mkvSize * 10 <= mp4Size * 9) {
            DeleteFileW(mkv.c_str());
            LOGE("job: MKV step: output %s is %llu bytes vs MP4 %llu bytes (< 90%%)", have ? "too small" : "missing", mkvSize, mp4Size);
            fail(L"the MKV is missing or smaller than 90% of the MP4");
            return;
        }
        mkvOk = true;
        LOGI("job: MKV step ok in %.1f s (%llu bytes, MP4 %llu bytes) -> %s", sec, mkvSize, mp4Size, N(FileName(mkv)).c_str());
    }

    if (cfg.postFlat) {
        SetStep(L"flat");
        bool nvenc = false;
        if (RunFfmpeg(Q(ff) + L" -hide_banner -encoders", out, code) && code == 0) nvenc = out.find("hevc_nvenc") != std::string::npos;
        wchar_t noNv[8];
        if (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_NO_NVENC", noNv, 8) > 0) nvenc = false;   // test-only: never touch the GPU
        const std::wstring common = Q(ff) + L" -hide_banner -loglevel error -y -i " + Q(mp4) +
            L" -map 0:v:0 -map 0:a? -vf \"crop=iw/2:ih:0:0,crop=iw:min(ih\\,iw*9/16),scale=3840:-2\" ";
        const std::wstring tail = L" -c:a copy -movflags +faststart " + Q(flat);
        const std::wstring encNv = L"-c:v hevc_nvenc -preset p5 -rc vbr -b:v 40M -maxrate 80M -tag:v hvc1";
        const std::wstring encX = L"-c:v libx264 -preset medium -crf 18";
        const ULONGLONG s0 = GetTickCount64();
        bool ran = false;
        code = (DWORD)-1;
        if (nvenc) {
            const std::wstring cmd = common + encNv + tail;
            LOGI("job: flat cmd (hevc_nvenc): %s", N(cmd).c_str());
            ran = RunFfmpeg(cmd, out, code);
            if (quitting()) { DeleteFileW(flat.c_str()); LOGW("job: cancelled (tray exiting)"); return; }
            if (!ran || code != 0) LOGW("job: hevc_nvenc failed (%s) -- retrying with libx264", N(FirstLine(out)).c_str());
        } else {
            LOGI("job: hevc_nvenc not listed by ffmpeg -- using libx264");
        }
        if (!ran || code != 0) {
            const std::wstring cmd = common + encX + tail;
            LOGI("job: flat cmd (libx264): %s", N(cmd).c_str());
            ran = RunFfmpeg(cmd, out, code);
            if (quitting()) { DeleteFileW(flat.c_str()); LOGW("job: cancelled (tray exiting)"); return; }
        }
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

    // Delete the MP4 only when the verified MKV replaces it and every requested step worked.
    std::wstring deleted;
    if (mkvOk && !cfg.keepMp4) {
        if (DeleteFileW(mp4.c_str())) { LOGI("job: deleted %s (keep_mp4 = 0)", N(name).c_str()); deleted = L" (MP4 removed)"; }
        else LOGW("job: could not delete %s (error %lu)", N(name).c_str(), GetLastError());
    }

    const std::wstring mainOut = mkvOk ? FileName(mkv) : FileName(flat);
    std::wstring text = mkvOk ? L"Ready for YouTube: " + FileName(mkv) : L"Flat 2D version ready: " + FileName(flat);
    if (mkvOk && flatOk) text += L"\nFlat 2D: " + FileName(flat);
    Notice n{L"OpenXR-EyeCapture", text, false};
    LOGI("job: done %s in %.1f s%s", N(mainOut).c_str(), (GetTickCount64() - t0) / 1000.0, N(deleted).c_str());
    Finish(true, L"last: " + mainOut + L" ready", &n);
}

} // namespace pp
