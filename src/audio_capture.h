// audio_capture -- WASAPI loopback of a render endpoint, delivered as a continuous 48 kHz stereo int16 stream on the
// QPC clock.
//
// Loopback only delivers packets while something is playing, and the device clock drifts slightly against QPC, so
// the stream is kept continuous and aligned here: each packet's QPC position (from GetBuffer) gives its target sample
// index on the 48 kHz timeline that starts at Start(); gaps over 10 ms are filled with silence, overlaps over 50 ms are
// trimmed, and while the endpoint is idle silence is generated up to (now - 100 ms). Output timestamps are therefore
// exactly t0 + n * 1e7 / 48000 (100-ns QPC units) and never jump.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class AudioCapture {
public:
    static const uint32_t kRate = 48000;
    static const uint32_t kChannels = 2;
    // frames = stereo frames; startQpc100 = QPC time (100 ns) of the first frame.
    using Sink = std::function<void(const int16_t* pcm, uint32_t frames, int64_t startQpc100)>;

    ~AudioCapture();
    // deviceSubstring: case-insensitive part of the endpoint's friendly name; empty = default render endpoint.
    bool Start(const std::wstring& deviceSubstring, Sink sink);
    void Stop();
    const std::string& DeviceName() const { return deviceName_; }
    const std::string& FormatText() const { return formatText_; }
    uint64_t FramesOut() const { return framesOut_.load(); }
    uint64_t SilenceFrames() const { return silenceFrames_.load(); }
    uint64_t DroppedFrames() const { return droppedFrames_.load(); }

private:
    void Run();
    void Emit(const int16_t* pcm, uint32_t frames);
    void EmitSilence(uint64_t frames);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    Sink sink_;
    std::string deviceName_, formatText_;
    std::wstring deviceSubstring_;
    int64_t t0_ = 0;            // QPC 100 ns of sample index 0
    uint64_t written_ = 0;      // 48 kHz frames emitted
    std::atomic<uint64_t> framesOut_{0}, silenceFrames_{0}, droppedFrames_{0};
    // init handshake
    std::atomic<int> initState_{0};   // 0 pending, 1 ok, -1 failed
};
