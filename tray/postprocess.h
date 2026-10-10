// postprocess -- the tray app's post-recording conversion worker.
//
// After the layer finishes an MP4, the tray queues it here. One worker thread runs the jobs one at a time, FIFO:
//   1. MKV  (post_mkv, default on): ffmpeg remux (no re-encode) to <name>.mkv with Matroska stereo_mode=left_right
//      (top_bottom for VR360, from the <name>.json sidecar), which is what YouTube reliably reads as 3D. Dome recordings:
//      ffmpeg's mov demuxer turns the MP4's sv3d box into spherical side data and the Matroska muxer writes it as the
//      Projection element, so the MKV is a VR180 / VR360 file as well. Verified (exists, > 90% of the MP4 size) before
//      anything is deleted.
//   2. flat (post_flat, default off): <name>_FLAT.mp4, left eye, 16:9 centre crop, 3840 wide (same as scripts\make_flat.ps1);
//      dome recordings: the left eye's view straight ahead rendered flat (v360), 3840x2160.
//  2b. 8K (post_yt8k, default off, flat SBS only; dome recordings log "skipped" and count as ok): <name>_YT8K.mkv,
//      each eye scaled (lanczos, aspect kept) into its own 3840x4320 box and padded there, hstacked to 7680x4320, so
//      YouTube keeps its 8K tier and bitrate; capped to 60 fps when ffprobe reports r_frame_rate > 60. hevc_nvenc
//      150 Mbit/s VBR, else libx265 crf 18 (slow). Matroska stereo_mode=left_right, written directly (no MP4 step).
//   3. the MP4 (and its sidecar) is deleted only if the MKV was verified and every requested step succeeded, and keep_mp4 = 0.
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
    std::wstring step;       // "MKV" / "flat" / "8K"
    size_t       queued = 0; // waiting behind the running job
    std::wstring lastLine;   // "last: x.mkv ready" / "last: x.mp4 conversion FAILED (see tray.log)"; empty = no job yet
    bool         lastOk = true;
};

// What a recording is, from the recorder's <name>.json sidecar next to the MP4 (dome recordings only).
// No sidecar = flat side-by-side (every recording made without dome mode).
struct Recording {
    enum Projection { Flat, Vr180, Vr360 } projection = Flat;
    bool         topBottom = false;   // layout top_bottom (vr360) -> Matroska stereo_mode=top_bottom
    bool         hasSidecar = false;
    std::wstring sidecar;             // <name>.json (path, whether or not it exists)
    bool Dome() const { return projection != Flat; }
    const wchar_t* Name() const;      // "3D SBS" / "VR180" / "VR360"
};
Recording ReadSidecar(const std::wstring& mp4);

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
