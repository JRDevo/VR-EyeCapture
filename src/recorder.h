// recorder -- one MP4 recording: shared SBS ring (via an IFrameSource) -> RGBA->NV12 on the GPU (D3D11 video
// processor) -> hardware HEVC / H.264 / AV1 through the Media Foundation sink writer, + WASAPI loopback -> AAC, then the
// 'st3d' box. Used unchanged by the layer and by tests/encoder_selftest.
//
// Threads:
//   * game thread  : SubmitFrame() -- picks a free ring slot, IFrameSource::CopyEyes (GPU work only), queues it.
//                    Never waits: a full ring (or a slot still busy on the GPU) drops the frame and counts it.
//   * worker thread: everything else (device + ring creation, sink writer, per-frame [linear->sRGB pre-pass],
//                    [head-motion smoothing re-projection | VR180 / VR360 dome pass], NV12 conversion and WriteSample,
//                    finalize, st3d (+ sv3d and a <name>.json sidecar for dome recordings; the sidecar also for YouTube-mode ones).
//                    One worker per recording.
//   * audio thread : AudioCapture -> WriteSample on the audio stream.
// The encoder device is private (created on the game's adapter); Media Foundation never touches the game's device.
#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "dome.h"
#include "frame_source.h"
#include "head_smoothing.h"
#include "overlay.h"
#include "video_codec.h"
#include "yt_layout.h"

struct RecorderSettings {
    std::wstring path;               // output .mp4
    VideoCodec   codec = VideoCodec::Hevc;   // AV1: hardware MFT only; falls back to HEVC when it cannot be set up
    unsigned     bitrate = 0;        // bits/s; 0 = 120 Mb/s HEVC, 80 Mb/s H.264, 80 Mb/s AV1 (VideoCodecDefaultMbps)
    RateControl  rateControl = RateControl::Vbr;   // vbr / cbr: at `bitrate`; qp: constant QP `qp`, `bitrate` ignored
    unsigned     qp = 24;            // 0..51 (rate_control = qp)
    int          qualityVsSpeed = -1;   // CODECAPI_AVEncCommonQualityVsSpeed 0..100; -1 = auto (AV1 above 1 Gpixel/s: 25)
    // constant_frame_rate: exactly one frame per 1/fps (fps = the nominal rate below, after max_fps) at k / fps with constant
    // durations: a frame that lands in an already written slot is dropped, a missing slot repeats the previous frame
    // (at most 5 s per gap). Off = the real timestamps (variable frame rate).
    bool         constantFrameRate = false;
    unsigned     fps = 90;           // nominal frame rate written to the stream (timestamps are the real ones)
    unsigned     maxFps = 0;         // 0 = keep every submitted frame
    unsigned     maxWidth = 0;       // 0 = native
    bool         audio = true;
    std::wstring audioDevice;
    HeadSmoothingSettings head;      // head-motion smoothing + symmetric fov (both off: re-projection stage bypassed)
    DomeSettings dome;               // projection = vr180 / vr360: dome output instead of the flat re-projection
    // record_for_youtube (yt_layout.h): flat recordings only -- the re-projection stage (symmetric fov, forced on) renders
    // both eyes cropped (eyeCrop), scaled (short side 4320; XR_EYE_CAPTURE_TEST_YT_SHORT overrides it) and packed (layout3d)
    // into the recorded frame; st3d top-bottom / left-right and a <name>.json sidecar ("youtube":1) so the tray only remuxes.
    bool         youtube = false;
    bool         tag3d = true;       // tag_3d: false = no st3d / sv3d boxes (a plain MP4; the sidecar is still written)
    Layout3d     layout3d = Layout3d::TbHalf;
    EyeCrop      eyeCrop = EyeCrop::Wide169;
    // The two views' fov (angleLeft, angleRight, angleUp, angleDown) and image sizes at record start: symmetric_fov
    // fixes the output frustum and size of the whole recording from these.
    bool         startFovValid = false;
    float        startFov[2][4] = {};
    uint32_t     startEyeW[2] = {}, startEyeH[2] = {};
};

enum class RecState { Idle, Starting, Recording, Stopping, Failed };
const char* RecStateName(RecState s);

struct RecorderStatus {
    RecState     state = RecState::Idle;
    unsigned     width = 0, height = 0, fps = 0;  // encoded size / nominal rate
    uint64_t     framesWritten = 0, framesDropped = 0;
    double       elapsedSec = 0;
    std::wstring file;
    std::string  message;                         // last error / result line
};

class Recorder {
public:
    Recorder();
    ~Recorder();

    // Starts the worker; returns false only if a previous recording is still finalizing.
    // w x h = SBS size (left eye width = leftWidth). fmt = typed staging format. srgbEncoded = data is gamma-encoded.
    bool Start(IFrameSource* src, const RecorderSettings& s, UINT w, UINT h, UINT leftWidth, DXGI_FORMAT fmt, bool srgbEncoded);
    // Game thread. ts100ns = presentation time on the QPC clock (100-ns units). L/R are clipped here.
    // displayTimeNs = the frame's XrFrameEndInfo::displayTime (head smoothing dt); 0 = derive from ts100ns.
    // ov (record_overlays): this frame's overlay layers, back to front, composited into the recorded eyes (overlay.h);
    // nullptr / n = 0 = none (no overlay work at all). Returns true if the frame was queued.
    bool SubmitFrame(int64_t ts100ns, const EyeView& L, const EyeView& R, int64_t displayTimeNs = 0,
                     const OverlayFrame* ov = nullptr);
    void Stop();                        // async; any thread
    bool WaitIdle(DWORD ms);            // waits for the worker to finish (finalize + st3d)
    RecState State() const { return state_.load(); }
    RecorderStatus Status();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<RecState> state_{RecState::Idle};
};
