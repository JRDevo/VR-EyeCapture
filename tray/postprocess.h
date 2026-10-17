// postprocess -- the tray app's post-recording conversion worker.
//
// After the layer finishes an MP4, the tray queues it here. One worker thread runs the jobs one at a time, FIFO:
//   1. MKV  (post_mkv, default on): <name>.mkv with a Matroska stereo_mode tag, which is what YouTube reliably reads as 3D.
//      layout_3d = sbs_full with eye_crop = none, and every dome recording: ffmpeg remux (no re-encode), stereo_mode=left_right (top_bottom for
//      VR360, from the <name>.json sidecar). Dome recordings: ffmpeg's mov demuxer turns the MP4's sv3d box into spherical
//      side data and the Matroska muxer writes it as the Projection element, so the MKV is a VR180 / VR360 file as well.
//      Verified (exists, > 90% of the MP4 size) before anything is deleted.
//      Recorded in the YouTube layout (record_for_youtube = 1: the sidecar has "youtube":1, "packing", "eye_crop"; see
//      Recording): ALWAYS the plain remux too, stereo_mode from the sidecar's "layout", whatever this job's layout_3d /
//      eye_crop say (they are baked into the picture); <name>.mkv, no crop tag; logged "recorded in the YouTube layout
//      (tb_half, 16:9, 7680x4320): remux only". The 8K step is skipped (logged, counts as ok, no _YT8K file), so keep_mp4 = 0
//      deletes the MP4 after the verified remux.
//      Flat recording with any other layout_3d (default tb_half): RE-ENCODE with the per-eye layout graph (see 2b) at the
//      native size, source fps kept, audio copied. Verified (exists, size > 0, ffprobe duration within 1 s / 2% of the MP4's)
//      before anything is deleted.
//      eye_crop (default 16:9; 4:3 | none): both re-encodes first centre-crop each eye vertically to that aspect when the eye is
//      taller than it (flat SBS eyes are often taller than wide, e.g. 4096x4630; tall top/bottom video crashes the Quest YouTube
//      player and YouTube caps it at 2160p). Never stretches. 16:9 + tb_half: 8K frame 7680x4320; 4:3: 5760x4320; 4:3pad:
//      the 4:3 crop, then black bars left and right (pad, in eye units before the scale) up to 16:9: 7680x4320 with the 4:3
//      picture 5760 wide in the middle (YouTube's 8K tier). sbs_full with a crop is therefore a re-encode too (only eye_crop =
//      none keeps the plain remux).
//   File names: a step whose picture depends on eye_crop gets a tag before the extension when the crop is not 16:9: "_4x3",
//      "_4x3pad" or "_full" (<name>_4x3.mkv, <name>_YT8K_4x3.mkv). Only the re-encoding MKV step is tagged (a plain remux ignores eye_crop);
//      _FLAT.mp4 is always 16:9 and never tagged. So converting one recording with two crops keeps both files. A conversion
//      never overwrites: if the name exists, the first free "<name> (2)", "(3)" ... is used (logged "exists -- writing ... instead").
//   2. flat (post_flat, default off): <name>_FLAT.mp4, left eye, 16:9 centre crop, 3840 wide (same as scripts\make_flat.ps1);
//      dome recordings: the left eye's view straight ahead rendered flat (v360), 3840x2160. Packed (YouTube-layout)
//      recordings: the left eye is taken from the top / left part and un-squeezed first (tb_half: crop=iw:ih/2:0:0,
//      scale=iw:ih*2; sbs_half: crop=iw/2:ih:0:0,scale=iw*2:ih; tb_full: crop=iw:ih/2:0:0; sbs_full as SBS), then the same
//      16:9 crop and 3840 scale.
//  2b. 8K (post_yt8k, default off, flat SBS only; dome and packed recordings log "skipped" and count as ok): <name>_YT8K.mkv,
//      each eye cropped and scaled (lanczos, no padding; BuildLayoutGraph) and stacked per layout_3d: tb_half (default, YouTube's
//      half-format: frame = one eye's aspect), sbs_half, sbs_full, tb_full; the frame's short side is 4320, its long side
//      <= 8192, so YouTube keeps its 8K tier and bitrate; capped to 60 fps when ffprobe reports r_frame_rate > 60.
//      hevc_nvenc 150 Mbit/s VBR (yt8k_bitrate_mbps), else libx265 crf 18 (slow); yt8k_codec = av1: av1_nvenc first (same
//      VBR maths, -tier 1 -highbitdepth 0 -multipass qres), falling back to hevc_nvenc, then libx265. Matroska stereo_mode
//      (top_bottom / left_right) written directly (no MP4 step).
//   AV1 recordings (codec = av1): the remux copies AV1 into the MKV; the NVENC re-encodes decode it with -hwaccel d3d11va
//      (ffmpeg's software AV1 decoder is libaom in the usual builds, ~2.5x slower at 8K), software re-encodes in software.
//   3. the MP4 (and its sidecar) is deleted only if the MKV was verified and every requested step succeeded, and keep_mp4 = 0.
//      Only a source whose name ends in .mp4 is ever deleted (last line of defence behind the tray's file picker, which
//      refuses anything but recordings). The sidecar counts only when <name>.json contains "projection" (the recorder's
//      keys), so an unrelated <name>.json is never deleted with the MP4.
//   Partial outputs: every step writes "<final name>.part" (ffmpeg gets -f matroska / -f mp4, the extension no longer says
//      the format), the step's verification probes the .part, and only a verified .part is renamed to its final name
//      (FreePath'd again at that moment). So a file with a final-looking name is always complete: a conversion killed with
//      the session (logoff, shutdown, the tray killed) leaves only a .part. Stale "<stem>*.mkv.part" / "*.mp4.part"
//      siblings of a job's MP4 are deleted when queue.json is restored (Start) and when that job starts; a cancel / exit
//      deletes the running step's .part. Log lines and balloons name the final files ("deleted partial" names the .part).
//   8K file name: also "_AV1" after the crop tag when the job's yt8k_codec = av1 AND av1_nvenc made it (<name>_YT8K_AV1.mkv,
//      <name>_YT8K_4x3_AV1.mkv); when av1_nvenc was not available and HEVC made the file, it is renamed to the untagged name
//      (first free one), so the tag never lies. HEVC (default) is never tagged.
// Per-job format (JobFormat): the conversion keys a job uses -- post_mkv, post_flat, post_yt8k, keep_mp4, layout_3d,
// eye_crop, yt8k_codec, yt8k_bitrate_mbps. A job added with a format (Settings window "Add to queue..." / "Convert a
// recording..." take the values shown in the window, saved or not; the tray menu's "Convert a recording..." takes
// config.ini's) carries that SNAPSHOT; a job without one (automatic recordings, "Convert last recording now") uses config.ini as
// it is when the job STARTS (the old behaviour: a settings change while a recording waits for the game to close still
// applies to it). Every other key (ffmpeg_path, ...) always comes from config.ini at job start. So one recording can be
// queued several times with different formats. Dedup: same path AND same effective format (a job without a snapshot counts
// as config.ini's current values).
// keep_mp4 = 0 never deletes an MP4 (or its sidecar) that another waiting job still needs (same path, any format): the
// last job for that MP4 applies its own keep_mp4.
// The UI thread never blocks: it polls GetState() / PopNotice().
//
// Hold (post_when, set by the tray every 500 ms through SetHold): while held, the worker does not START the next job --
// jobs stay queued, FIFO. If the hold turns on while a job's ffmpeg runs, that ffmpeg is SUSPENDED (NtSuspendProcess) and
// resumed when the hold clears; a job between two ffmpeg runs waits before it starts the next one. Forced jobs
// (Enqueue(.., true): "Convert last recording now", "Convert a recording...", Settings "Start" / "Start all") ignore the hold:
// the worker always picks the FORCED job that was forced first (Start = "next"), so a forced job is next even when the queue
// is not held, and they are
// never suspended. Stop() resumes a suspended ffmpeg before it terminates it. post_when = manual is a hold that never clears:
// nothing starts until a request forces it (Enqueue(.., true) also upgrades a matching waiting or running job).
// Held by the user (Enqueue(.., userHeld = true): Settings "Add to queue..."; every job restored from queue.json): the job
// waits whatever post_when says (also with post_when = now) until Start(id) / StartAll() force it. Queued() lists the
// waiting jobs (with ids) for the Settings window's queue view; Remove(id) drops one.
// Persistence: the running job (first) and the waiting jobs are written to <exeDir>\queue.json after every change (atomic
// replace, flushed before the rename). Start() reads it back: every job whose MP4 still exists returns HELD BY THE USER (never
// starts on its own after a restart; the producer game / session state is gone), keeping its format snapshot; missing MP4s
// are dropped (logged).
// Finished steps of the running job are persisted too: after each verified step the worker records its output file name
// ("done_mkv" / "done_flat" / "done_yt8k", file name only, next to the MP4) and saves queue.json; the first finished step
// also freezes the job's effective format as its snapshot (own_format 1), so the restored job converts with the same
// settings. A restored job skips a step whose recorded file still exists (logged "already done"; it counts as ok for the
// keep_mp4 decision and names that file in the balloon) and redoes it when the file is gone.
// The worker skips the final SaveQueue only when RunJob reports it was INTERRUPTED by Stop() (tray exiting before the job
// completed); a job that completed (or failed, or was cancelled) while Exit was pending leaves queue.json.
// Child processes: every ffmpeg / ffprobe is started CREATE_SUSPENDED, put in one job object (created by Start(),
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) and then resumed, so a tray that crashes or is killed takes its ffmpeg with it
// (never an orphaned, possibly hold-suspended ffmpeg holding the MP4 and an NVENC session). If the assignment fails (logged
// once) the child runs as before.
// Low-space guard (src/disk.h): once the source duration is known (ffprobe), a job does not start unless the MP4's drive
// has MinFreeToStart() (10 GB) + the estimated output: duration x the steps' video bitrates / 8 (MKV re-encode 150 Mbit/s,
// MKV remux = the MP4's size, 8K = yt8k_bitrate_mbps, flat 40 Mbit/s; a step already done is not counted; without a
// duration a re-encode step counts as the MP4's size). Logged and in the balloon as "needs about N GB".
//
// Progress: every conversion ffmpeg (MKV, flat, 8K) runs with "-nostats -progress pipe:1". RunFfmpeg then reads ffmpeg's
// stdout (the key=value progress blocks, about every 0.5 s) and stderr (errors; the first line is the failure reason, as
// before) on two pipes; out_time_us against the source duration (ffprobe, once per job) gives State::percent, and the
// rate of media time per ACTIVE second (time suspended is not counted) gives State::etaSec. State is updated and the UI
// poked at most once per second; the log gets one line per 25% of a step. Without ffprobe the percent stays unknown (-1).
// User pause (TogglePause, tray menu "Pause conversion" / WM_APP+5): the running job's ffmpeg is suspended exactly like
// the hold (and a job between two ffmpeg runs waits); independent of the hold and also applies to forced jobs.
// Cancel (tray menu "Cancel conversion" / WM_APP+6): resumes, then terminates the running ffmpeg, deletes that step's
// partial output (its .part), skips the job's remaining steps, never deletes the MP4 (keep_mp4 is not applied), logs
// "job: cancelled by user ..." and queues the notice "Conversion cancelled: <file>". Not a failure (lastOk stays true).
// Queued jobs continue.
#pragma once
#include <windows.h>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "config.h"

namespace pp {

// YouTube plays at most 60 fps and the 8K conversion makes 60 fps, so a game frame rate that 60 does not divide evenly (72, 80,
// 90, 144 ...) gives uneven motion. "Even" = 60, 120, 180, 240 (60 divides it) or 30, 20, 15 (it divides 60), within +-1.5 fps
// of the measured value. fps <= 0 (unknown) is not flagged.
inline bool UnevenFor60(double fps) {
    if (!(fps > 0)) return false;
    static const double even[] = {15, 20, 30, 60, 120, 180, 240};
    for (double e : even) if (std::fabs(fps - e) <= 1.5) return false;
    return true;
}
// The one-line warning shown in the window; shortForm (tray tooltip, narrow window) is shorter. "" when the rate is fine / unknown.
inline std::wstring FpsWarningText(double fps, bool shortForm = false) {
    if (!UnevenFor60(fps)) return L"";
    return std::to_wstring((int)(fps + 0.5)) + (shortForm ? L" fps: YouTube video will stutter, use 120 Hz"
                                                          : L" fps: the 60 fps YouTube video will stutter -- set the headset to 120 Hz");
}

// eye_crop target aspect as N:D (0 = no crop). 4:3pad crops to 4:3 (then pads, see PredictLayout).
inline void EyeAspect(EyeCrop c, int& n, int& d) {
    const bool c43 = c == EyeCrop::Std43 || c == EyeCrop::Pad43;
    n = c == EyeCrop::Wide169 ? 16 : c43 ? 4 : 0;
    d = c == EyeCrop::Wide169 ? 9 : c43 ? 3 : 1;
}
// The pure size arithmetic of the per-eye layout graph (BuildLayoutGraph), shared by the log line (PredictFrame) and the Settings
// preview so they cannot drift. Input: one eye of the SBS recording (eyeW x eyeH). shortSide 0 = native size, else the frame's short side.
//   cw x ch   the eye after eye_crop (centre crop, even height)
//   pw        4:3pad: the eye's width after the black bars, pw = max(cw, 2*round(ch*16/9/2)) (even; rounded so a Quest 3
//             eye, ch 3072, gives 5462 -> 7680 wide), bars padX = 2*floor((pw-cw)/4) on the left; = cw for every other crop.
//             From here on the arithmetic uses pw for the eye width.
//   fw x fh   the frame ffmpeg makes
//   sw x sh   one eye as stored in the frame (squeezed in the half layouts)
//   vw, vx    the picture inside it: width and x offset (4:3pad; else sw and 0). ffmpeg scales the padded eye, so its picture
//             edge may sit up to ~1 px from vx / vx + vw.
// src/yt_layout.h (PredictYtLayout) has the same arithmetic for record_for_youtube: change both.
struct LayoutSize { int cw = 0, ch = 0, fw = 0, fh = 0, sw = 0, sh = 0, pw = 0, vw = 0, vx = 0; };
inline LayoutSize PredictLayout(int eyeW, int eyeH, Layout3d lay, EyeCrop crop, int shortSide) {
    LayoutSize o;
    o.cw = eyeW;
    o.ch = eyeH;
    int n, d;
    EyeAspect(crop, n, d);
    if (n > 0) {
        const int want = 2 * (int)std::floor((double)o.cw * d / n / 2.0);
        if (want < o.ch) o.ch = want;
    }
    o.pw = o.cw;
    if (crop == EyeCrop::Pad43) {
        const int padded = 2 * (int)std::floor((double)o.ch * 16 / 9 / 2.0 + 0.5);
        if (padded > o.pw) o.pw = padded;
    }
    const int padX = 2 * ((o.pw - o.cw) / 4);
    const double ew = (double)(lay == Layout3d::SbsFull ? 2 * o.pw : o.pw), eh = (double)(lay == Layout3d::TbFull ? 2 * o.ch : o.ch);
    const double longCap = 8192.0 / (ew > eh ? ew : eh);
    double sc = longCap;
    if (shortSide > 0) {
        const double s = (double)shortSide / (ew < eh ? ew : eh);
        if (s < sc) sc = s;
    } else if (sc > 1.0) {
        sc = 1.0;
    }
    const double wDiv = lay == Layout3d::SbsHalf ? 4.0 : 2.0, hDiv = lay == Layout3d::TbHalf ? 4.0 : 2.0;
    o.sw = 2 * (int)std::floor(o.pw * sc / wDiv + 0.5);
    o.sh = 2 * (int)std::floor(o.ch * sc / hDiv + 0.5);
    o.vw = 2 * (int)std::floor(o.cw * sc / wDiv + 0.5);
    o.vx = 2 * (int)std::floor(padX * sc / wDiv + 0.5);
    const bool sbs = lay == Layout3d::SbsHalf || lay == Layout3d::SbsFull;
    o.fw = sbs ? 2 * o.sw : o.sw;
    o.fh = sbs ? o.sh : 2 * o.sh;
    return o;
}

// "about 6 min left" for an ETA in seconds ("" when unknown). Shared by the tray tooltip / menu and the Settings queue view.
inline std::wstring EtaText(int s) {
    if (s < 0) return L"";
    if (s < 60) return L"about " + std::to_wstring(s < 5 ? 5 : (s + 4) / 5 * 5) + L" s left";
    if (s < 3600) return L"about " + std::to_wstring((s + 30) / 60) + L" min left";
    const int m = (s + 30) / 60;
    return L"about " + std::to_wstring(m / 60) + L" h " + std::to_wstring(m % 60) + L" min left";
}

struct Notice {
    std::wstring title, text;
    bool error = false;
};

// The conversion keys of one job (see the header comment). Default-constructed = Config's defaults.
struct JobFormat {
    bool       postMkv = true, postFlat = false, postYt8k = false, keepMp4 = false;
    Layout3d   layout3d = Layout3d::TbHalf;
    EyeCrop    eyeCrop = EyeCrop::Wide169;
    VideoCodec yt8kCodec = VideoCodec::Hevc;
    unsigned   yt8kBitrateMbps = 150;
    bool operator==(const JobFormat& o) const {
        return postMkv == o.postMkv && postFlat == o.postFlat && postYt8k == o.postYt8k && keepMp4 == o.keepMp4 &&
               layout3d == o.layout3d && eyeCrop == o.eyeCrop && yt8kCodec == o.yt8kCodec && yt8kBitrateMbps == o.yt8kBitrateMbps;
    }
    bool operator!=(const JobFormat& o) const { return !(*this == o); }
    bool Empty() const { return !postMkv && !postFlat && !postYt8k; }   // nothing to make
};
// Inline: settings.cpp is also linked into settings_test without postprocess.cpp.
inline JobFormat FormatFromConfig(const Config& c) {
    JobFormat f;
    f.postMkv = c.postMkv;
    f.postFlat = c.postFlat;
    f.postYt8k = c.postYt8k;
    f.keepMp4 = c.keepMp4;
    f.layout3d = c.layout3d;
    f.eyeCrop = c.eyeCrop;
    f.yt8kCodec = c.yt8kCodec == VideoCodec::Av1 ? VideoCodec::Av1 : VideoCodec::Hevc;
    f.yt8kBitrateMbps = c.yt8kBitrateMbps;
    return f;
}
inline void ApplyFormat(const JobFormat& f, Config& c) {
    c.postMkv = f.postMkv;
    c.postFlat = f.postFlat;
    c.postYt8k = f.postYt8k;
    c.keepMp4 = f.keepMp4;
    c.layout3d = f.layout3d;
    c.eyeCrop = f.eyeCrop;
    c.yt8kCodec = f.yt8kCodec;
    c.yt8kBitrateMbps = f.yt8kBitrateMbps;
}
inline const wchar_t* LayoutName(Layout3d l) {   // "tb_half" ...
    return l == Layout3d::SbsHalf ? L"sbs_half" : l == Layout3d::SbsFull ? L"sbs_full" : l == Layout3d::TbFull ? L"tb_full" : L"tb_half";
}
inline const wchar_t* CropName(EyeCrop c) {   // the eye_crop value
    return c == EyeCrop::Std43 ? L"4:3" : c == EyeCrop::Pad43 ? L"4:3pad" : c == EyeCrop::None ? L"none" : L"16:9";
}
// "4:3pad" / "4:3" / "none" / "16:9" (sidecar, queue.json) -> EyeCrop; anything else = 16:9.
inline EyeCrop CropFromName(const std::string& s) {
    return s == "4:3" ? EyeCrop::Std43 : s == "4:3pad" ? EyeCrop::Pad43 : s == "none" ? EyeCrop::None : EyeCrop::Wide169;
}
// "MKV + 8K AV1 100M + 2D, 4:3, tb_half" (+ ", keep MP4"); "nothing to make" when Empty(). For the queue view / logs.
inline std::wstring FormatText(const JobFormat& f) {
    if (f.Empty()) return L"nothing to make";
    std::wstring s;
    auto add = [&](const std::wstring& part) { s += (s.empty() ? L"" : L" + ") + part; };
    if (f.postMkv) add(L"MKV");
    if (f.postYt8k)
        add(std::wstring(L"8K ") + (f.yt8kCodec == VideoCodec::Av1 ? L"AV1 " : L"HEVC ") + std::to_wstring(f.yt8kBitrateMbps) + L"M");
    if (f.postFlat) add(L"2D");
    if (f.postMkv || f.postYt8k) s += std::wstring(L", ") + CropName(f.eyeCrop) + L", " + LayoutName(f.layout3d);
    if (f.keepMp4) s += L", keep MP4";
    return s;
}

struct State {
    bool         busy = false;
    std::wstring file;       // file name of the MP4 being processed
    std::wstring path;       // full path of the MP4 being processed (empty when idle)
    bool         forced = false;   // the running job is forced (ignores the hold)
    unsigned long long runningId = 0;   // id of the running job (0 = idle); Start(runningId) forces a held running job
    std::wstring format;     // FormatText of the running job's effective format ("" when idle)
    std::wstring step;       // "MKV" / "flat" / "8K"
    size_t       queued = 0; // waiting behind the running job
    size_t       userHeld = 0;     // of those: held by the user ("Add to queue...", restored), wait for Start whatever post_when says
    bool         held = false;     // SetHold(true): non-forced jobs do not start / a running non-forced ffmpeg is suspended
    bool         paused = false;   // the running job's ffmpeg is suspended (or waits to start): hold or user pause
    bool         userPaused = false;   // paused by the user (TogglePause)
    bool         cancelling = false;   // Cancel() was called; the job is cleaning up
    int          percent = -1;     // progress of the running ffmpeg step, 0..100; -1 = unknown / not started
    int          etaSec = -1;      // estimated seconds left in the running step (paused time not counted); -1 = unknown
    std::wstring holdReason;       // SetHold's reason ("when the game closes" / "when VR stops"); empty when not held
    std::wstring lastLine;   // "last: x.mkv ready" / "last: x.mp4 conversion FAILED (see tray.log)"; empty = no job yet
    bool         lastOk = true;
};

// A job waiting in the queue (Processor::Queued, oldest first). force = runs even while held (next); userHeld = waits for
// Start / Start all whatever post_when says. ownFormat = fmt is the job's snapshot; else the job uses config.ini at its
// start and fmt is left default (the tray fills in the current config.ini values for the queue view).
struct QueuedJob {
    unsigned long long id = 0;
    std::wstring path;
    bool         force = false;
    unsigned long long forceSeq = 0;   // forced jobs run in this order (when they were forced), before every other job
    bool         userHeld = false;
    bool         ownFormat = false;
    JobFormat    fmt;
};

// What a recording is, from the recorder's <name>.json sidecar next to the MP4 (dome recordings, and flat recordings made
// with record_for_youtube = 1). No sidecar = flat side-by-side (every other recording).
//   dome:    {"projection":"vr180"|"vr360","layout":"left_right"|"top_bottom"}
//   packed:  {"projection":"flat","layout":"top_bottom"|"left_right","packing":"tb_half"|"sbs_half"|"sbs_full"|"tb_full",
//             "eye_crop":"16:9"|"4:3"|"4:3pad"|"none","eyes":2,"width":W,"height":H,"youtube":1}
//            = recorded straight in the YouTube layout (crop and layout baked in): the tray only remuxes it.
// hasSidecar = the file contains "projection" (the recorder's keys), so an unrelated <name>.json is neither read nor deleted.
struct Recording {
    enum Projection { Flat, Vr180, Vr360 } projection = Flat;
    bool         topBottom = false;   // layout top_bottom (vr360, tb_half / tb_full packing) -> Matroska stereo_mode=top_bottom
    bool         hasSidecar = false;
    bool         packed = false;      // flat + "youtube":1: already in the YouTube layout (packing / crop below)
    Layout3d     packing = Layout3d::TbHalf;   // packed only: the layout_3d the layer recorded in
    EyeCrop      crop = EyeCrop::Wide169;      // packed only: the eye_crop it applied
    int          width = 0, height = 0;        // packed only: the frame size the sidecar states (0 = not given)
    std::wstring sidecar;             // <name>.json (path, whether or not it exists)
    bool Dome() const { return projection != Flat; }
    const wchar_t* Name() const;      // "3D SBS" / "VR180" / "VR360" / "YouTube tb_half" (packed)
};
Recording ReadSidecar(const std::wstring& mp4);

// ffmpeg lookup: cfg.ffmpegPath if set; else ffmpeg on PATH, else a recursive search of
// %LOCALAPPDATA%\Microsoft\WinGet\Packages, else <exeDir>\ffmpeg\ffmpeg.exe. Returns "" if none; `how` says where it came from.
std::wstring ResolveFfmpeg(const Config& cfg, const std::wstring& exeDir, std::wstring& how);

// "Tag a video as 3D" (tray menu, `EyeCaptureTray.exe --tag3d <sbs|tb> <file>`): a plain remux (-map 0 -c copy, no re-encode) of a
// video made elsewhere (e.g. an OBS side-by-side recording) into <name>_3D.mkv next to it, with the Matroska stereo_mode tag
// (left_right / top_bottom) that YouTube reads as 3D. Never overwrites ("<name>_3D (2).mkv" ...); written as .part and renamed
// once ffprobe (when found) agrees on the duration. Synchronous, self-contained (its own ffmpeg process, not the conversion
// queue): the caller runs it off the UI thread. True = `outPath` is the finished file; false = `why` says what went wrong.
// Logs the command and the result (LOGI / LOGE).
bool Tag3d(const Config& cfg, const std::wstring& exeDir, const std::wstring& in, bool topBottom, std::wstring& outPath, std::wstring& why);

class Processor {
public:
    ~Processor() { Stop(); }
    // `wnd` receives `msg` (no parameters) whenever the state or the notice queue changes.
    void Start(const std::wstring& exeDir, HWND wnd, UINT msg);
    void Stop();                                  // cancels a running ffmpeg (partial output deleted) and joins
    // false if a job for the same path with the same effective format is already queued / running. force = run even while
    // held, next (a matching waiting / running job is upgraded). fmt = the job's format snapshot (nullptr = config.ini at job
    // start). userHeld = wait for Start / StartAll whatever post_when says (ignored with force).
    bool Enqueue(const std::wstring& mp4, bool force = false, const JobFormat* fmt = nullptr, bool userHeld = false);
    // Forces one job: a waiting one (it runs next, also when held by the user) or the running one (no longer suspended by
    // the hold). False when the id is neither.
    bool StartJob(unsigned long long id);
    // Forces every waiting job; they run one after another, oldest first. Returns how many were changed.
    size_t StartAll();
    // Hold / release the queue (see the header comment). Thread-safe; cheap when nothing changes. `reason` is shown in
    // the tooltip ("converts <reason>").
    void SetHold(bool hold, const std::wstring& reason);
    State GetState();
    // Snapshot of the waiting jobs (not the running one), oldest first. Thread-safe.
    std::vector<QueuedJob> Queued();
    // Drops a waiting job from the queue (never the running one: use Cancel). False when it is not queued.
    bool Remove(unsigned long long id);
    bool PopNotice(Notice& n);
    std::wstring LastQueued();                    // full path of the most recently queued MP4 ("" = none)
    // User pause / resume of the running job. False when no job runs (or it is being cancelled); `paused` = the new state.
    bool TogglePause(bool& paused);
    // Cancels the running job (see the header comment). False when no job runs. Thread-safe, returns at once.
    bool Cancel();

private:
    struct Job {
        unsigned long long id = 0;
        std::wstring path;
        bool force = false, userHeld = false, ownFormat = false;
        unsigned long long forceSeq = 0;   // order of forcing: Start = "next" even behind other forced jobs
        JobFormat fmt;
        // steps finished in an earlier run (restored from queue.json) / so far (runningJob_): the output's FILE NAME, "" = not done
        std::wstring doneMkv, doneFlat, doneYt8k;
    };

    void Worker();
    bool RunJob(const Job& job);                  // true = interrupted by Stop() before it completed (stays in queue.json)
    void StepDone(const char* step, const std::wstring& file);   // records a finished step in runningJob_ + queue.json
    void SaveQueue();                             // writes queue.json (running job first, then the waiting ones)
    void LoadQueue();                             // Start(): restores queue.json, every job held by the user
    // stdout+stderr merged into `output`. progress = the command has "-progress pipe:1": stdout is parsed for progress
    // (durSec = source duration, 0 = unknown) and only stderr goes to `output`.
    bool RunFfmpeg(const std::wstring& cmdline, std::string& output, DWORD& exitCode, bool progress = false, double durSec = 0.0);
    void OnProgress(double doneSec, double durSec, bool end);
    void SetStep(const wchar_t* step);
    void Finish(bool ok, const std::wstring& lastLine, const Notice* notice);
    void Poke();
    bool StartableLocked() const;                 // a queued job may start now (caller holds m_)
    std::deque<Job>::iterator PickLocked();       // the job the worker starts next (end() = none); caller holds m_
    bool HoldPausedLocked() const { return hold_ && busy_ && !runningForced_; }
    bool PausedLocked() const { return busy_ && (userPause_ || HoldPausedLocked()); }
    void SuspendChildLocked(bool suspend, const char* why);   // NtSuspendProcess / NtResumeProcess on child_ (caller holds m_)
    void ApplySuspendLocked(const char* why) { SuspendChildLocked(PausedLocked(), why); }
    double ActiveSecLocked() const;               // seconds the running ffmpeg has run, suspended time excluded

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
    Job                     runningJob_;          // the running job as queued (persisted first in queue.json)
    JobFormat               runningFmt_;          // its effective format (snapshot, or config.ini at its start)
    unsigned long long      nextId_ = 1;
    unsigned long long      nextForceSeq_ = 1;
    std::mutex              saveM_;               // serialises SaveQueue (the snapshot is taken under it, so the newest wins)
    std::wstring            lastQueued_;
    HANDLE                  child_ = nullptr;     // running ffmpeg, guarded by m_
    HANDLE                  jobObj_ = nullptr;    // job object (kill on close) every child is assigned to; Start() / Stop()
    bool                    assignWarned_ = false;   // AssignProcessToJobObject failure logged once (worker thread only)
    bool                    hold_ = false;        // SetHold
    std::wstring            holdReason_;
    bool                    busy_ = false;        // a job is running
    bool                    runningForced_ = false;   // the running job was forced (ignores the hold)
    bool                    childSuspended_ = false;  // child_ is suspended by us
    bool                    userPause_ = false;   // TogglePause (reset per job)
    bool                    cancel_ = false;      // Cancel (reset per job)
    ULONGLONG               childStart_ = 0;      // GetTickCount64 when child_ started
    ULONGLONG               suspendStart_ = 0;    // when child_ was last suspended
    ULONGLONG               suspendedMs_ = 0;     // total suspended time of child_ (finished suspensions)
    ULONGLONG               lastProgressPoke_ = 0;
    int                     lastLoggedQuarter_ = 0;   // progress log: highest 25% mark logged for this ffmpeg run
};

} // namespace pp
