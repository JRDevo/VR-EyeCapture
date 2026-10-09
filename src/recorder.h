// recorder -- one MP4 recording: shared SBS ring (via an IFrameSource) -> RGBA->NV12 on the GPU (D3D11 video
// processor) -> hardware HEVC / H.264 through the Media Foundation sink writer, + WASAPI loopback -> AAC, then the
// 'st3d' box. Used unchanged by the layer and by tests/encoder_selftest.
//
// Threads:
//   * game thread  : SubmitFrame() -- picks a free ring slot, IFrameSource::CopyEyes (GPU work only), queues it.
//                    Never waits: a full ring (or a slot still busy on the GPU) drops the frame and counts it.
//   * worker thread: everything else (device + ring creation, sink writer, per-frame [linear->sRGB pre-pass],
//                    [head-motion smoothing re-projection], NV12 conversion and WriteSample, finalize, st3d).
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
#include "frame_source.h"
#include "head_smoothing.h"

struct RecorderSettings {
    std::wstring path;               // output .mp4
    bool         hevc = true;
    unsigned     bitrate = 0;        // bits/s; 0 = 120 Mb/s HEVC, 80 Mb/s H.264
    unsigned     fps = 90;           // nominal frame rate written to the stream (timestamps are the real ones)
    unsigned     maxFps = 0;         // 0 = keep every submitted frame
    unsigned     maxWidth = 0;       // 0 = native
    bool         audio = true;
    std::wstring audioDevice;
    HeadSmoothingSettings head;      // head-motion smoothing (head.smoothing = 0: stage bypassed)
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
    // Returns true if the frame was queued.
    bool SubmitFrame(int64_t ts100ns, const EyeView& L, const EyeView& R, int64_t displayTimeNs = 0);
    void Stop();                        // async; any thread
    bool WaitIdle(DWORD ms);            // waits for the worker to finish (finalize + st3d)
    RecState State() const { return state_.load(); }
    RecorderStatus Status();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<RecState> state_{RecState::Idle};
};
