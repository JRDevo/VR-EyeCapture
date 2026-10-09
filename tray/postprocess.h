// postprocess -- the tray app's post-recording conversion worker.
//
// After the layer finishes an MP4, the tray queues it here. One worker thread runs the jobs one at a time, FIFO:
//   1. MKV  (post_mkv, default on): ffmpeg remux (no re-encode) to <name>.mkv with Matroska stereo_mode=left_right,
//      which is what YouTube reliably reads as 3D. Verified (exists, > 90% of the MP4 size) before anything is deleted.
//   2. flat (post_flat, default off): <name>_FLAT.mp4, left eye, 16:9 centre crop, 3840 wide (same as scripts\make_flat.ps1).
//   3. the MP4 is deleted only if the MKV was verified and every requested step succeeded, and keep_mp4 = 0.
// Config is re-read at the start of every job. The UI thread never blocks: it polls GetState() / PopNotice().
#pragma once
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include "config.h"

namespace pp {

struct Notice {
    std::wstring title, text;
    bool error = false;
};

struct State {
    bool         busy = false;
    std::wstring file;       // file name of the MP4 being processed
    std::wstring step;       // "MKV" / "flat"
    size_t       queued = 0; // waiting behind the running job
    std::wstring lastLine;   // "last: x.mkv ready" / "last: x.mp4 conversion FAILED (see tray.log)"; empty = no job yet
    bool         lastOk = true;
};

// ffmpeg lookup: cfg.ffmpegPath if set; else ffmpeg on PATH, else a recursive search of
// %LOCALAPPDATA%\Microsoft\WinGet\Packages, else <exeDir>\ffmpeg\ffmpeg.exe. Returns "" if none; `how` says where it came from.
std::wstring ResolveFfmpeg(const Config& cfg, const std::wstring& exeDir, std::wstring& how);

class Processor {
public:
    ~Processor() { Stop(); }
    // `wnd` receives `msg` (no parameters) whenever the state or the notice queue changes.
    void Start(const std::wstring& exeDir, HWND wnd, UINT msg);
    void Stop();                                  // cancels a running ffmpeg (partial output deleted) and joins
    bool Enqueue(const std::wstring& mp4);        // false if it is already queued / running
    State GetState();
    bool PopNotice(Notice& n);
    std::wstring LastQueued();                    // full path of the most recently queued MP4 ("" = none)

private:
    void Worker();
    void RunJob(const std::wstring& mp4);
    bool RunFfmpeg(const std::wstring& cmdline, std::string& output, DWORD& exitCode);
    void SetStep(const wchar_t* step);
    void Finish(bool ok, const std::wstring& lastLine, const Notice* notice);
    void Poke();

    std::wstring            exeDir_;
    HWND                    wnd_ = nullptr;
    UINT                    msg_ = 0;
    std::thread             thread_;
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    quit_ = false;
    std::deque<std::wstring> queue_;
    std::deque<Notice>      notices_;
    State                   st_;
    std::wstring            running_;             // full path of the running job
    std::wstring            lastQueued_;
    HANDLE                  child_ = nullptr;     // running ffmpeg, guarded by m_
};

} // namespace pp
