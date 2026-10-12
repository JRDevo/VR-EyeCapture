// postprocess -- the tray app's post-recording conversion worker.
//
// After the layer finishes an MP4, the tray queues it here. One worker thread runs the jobs one at a time, FIFO:
//   1. MKV  (post_mkv, default on): <name>.mkv with a Matroska stereo_mode tag, which is what YouTube reliably reads as 3D.
//      layout_3d = sbs_full with eye_crop = none, and every dome recording: ffmpeg remux (no re-encode), stereo_mode=left_right (top_bottom for
//      VR360, from the <name>.json sidecar). Dome recordings: ffmpeg's mov demuxer turns the MP4's sv3d box into spherical
//      side data and the Matroska muxer writes it as the Projection element, so the MKV is a VR180 / VR360 file as well.
//      Verified (exists, > 90% of the MP4 size) before anything is deleted.
//      Flat recording with any other layout_3d (default tb_half): RE-ENCODE with the per-eye layout graph (see 2b) at the
//      native size, source fps kept, audio copied. Verified (exists, size > 0, ffprobe duration within 1 s / 2% of the MP4's)
//      before anything is deleted.
//      eye_crop (default 16:9; 4:3 | none): both re-encodes first centre-crop each eye vertically to that aspect when the eye is
//      taller than it (flat SBS eyes are often taller than wide, e.g. 4096x4630; tall top/bottom video crashes the Quest YouTube
//      player and YouTube caps it at 2160p). Never stretches. 16:9 + tb_half: 8K frame 7680x4320; 4:3: 5760x4320.
//      sbs_full with a crop is therefore a re-encode too (only eye_crop = none keeps the plain remux).
//   2. flat (post_flat, default off): <name>_FLAT.mp4, left eye, 16:9 centre crop, 3840 wide (same as scripts\make_flat.ps1);
//      dome recordings: the left eye's view straight ahead rendered flat (v360), 3840x2160.
//  2b. 8K (post_yt8k, default off, flat SBS only; dome recordings log "skipped" and count as ok): <name>_YT8K.mkv,
//      each eye cropped and scaled (lanczos, no padding; BuildLayoutGraph) and stacked per layout_3d: tb_half (default, YouTube's
//      half-format: frame = one eye's aspect), sbs_half, sbs_full, tb_full; the frame's short side is 4320, its long side
//      <= 8192, so YouTube keeps its 8K tier and bitrate; capped to 60 fps when ffprobe reports r_frame_rate > 60.
//      hevc_nvenc 150 Mbit/s VBR, else libx265 crf 18 (slow). Matroska stereo_mode (top_bottom / left_right) written
//      directly (no MP4 step).
//   3. the MP4 (and its sidecar) is deleted only if the MKV was verified and every requested step succeeded, and keep_mp4 = 0.
// Config is re-read at the start of every job. The UI thread never blocks: it polls GetState() / PopNotice().
//
// Hold (post_when, set by the tray every 500 ms through SetHold): while held, the worker does not START the next job --
// jobs stay queued, FIFO. If the hold turns on while a job's ffmpeg runs, that ffmpeg is SUSPENDED (NtSuspendProcess) and
// resumed when the hold clears; a job between two ffmpeg runs waits before it starts the next one. Forced jobs
// (Enqueue(.., true): "Convert last recording now") ignore the hold: they are picked from the queue ahead of held jobs and
// are never suspended. Stop() resumes a suspended ffmpeg before it terminates it.
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
    bool         held = false;     // SetHold(true): non-forced jobs do not start / a running non-forced ffmpeg is suspended
    bool         paused = false;   // the running job's ffmpeg is suspended (or waits to start) because of the hold
    std::wstring holdReason;       // SetHold's reason ("when the game closes" / "when VR stops"); empty when not held
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
    // false if it is already queued / running. force = run even while held (a queued non-forced copy is upgraded).
    bool Enqueue(const std::wstring& mp4, bool force = false);
    // Hold / release the queue (see the header comment). Thread-safe; cheap when nothing changes. `reason` is shown in
    // the tooltip ("converts <reason>").
    void SetHold(bool hold, const std::wstring& reason);
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
    bool StartableLocked() const;                 // a queued job may start now (caller holds m_)
    bool PausedLocked() const { return hold_ && busy_ && !runningForced_; }
    void SuspendChildLocked(bool suspend);        // NtSuspendProcess / NtResumeProcess on child_ (caller holds m_)

    struct Job { std::wstring path; bool force = false; };

    std::wstring            exeDir_;
    HWND                    wnd_ = nullptr;
    UINT                    msg_ = 0;
    std::thread             thread_;
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    quit_ = false;
    std::deque<Job>         queue_;
    std::deque<Notice>      notices_;
    State                   st_;
    std::wstring            running_;             // full path of the running job
    std::wstring            lastQueued_;
    HANDLE                  child_ = nullptr;     // running ffmpeg, guarded by m_
    bool                    hold_ = false;        // SetHold
    std::wstring            holdReason_;
    bool                    busy_ = false;        // a job is running
    bool                    runningForced_ = false;   // the running job was forced (ignores the hold)
    bool                    childSuspended_ = false;  // child_ is suspended by us
};

} // namespace pp
