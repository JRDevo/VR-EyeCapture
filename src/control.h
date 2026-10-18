// control -- the layer's control thread: hotkey polling (GetAsyncKeyState, works while the game has focus), commands
// from EyeCaptureTray.exe through shared memory, status publishing for the tray, and the audible cues.
//
// Cues (PlaySound from this process = its own audio session on the default device, so a headset on Virtual Desktop /
// Link hears it; Beep() went through the often-muted "System sounds" session):
//   start OK       1 tone  880 Hz
//   stop           2 tones 660 Hz
//   failed / none  1 long low tone 220 Hz (no capture-capable session, start failed, or no frames for 6 s)
// config beeps = 0 silences all of them (the cues play on the default device, which is often the WASAPI loopback device the
// recording captures, so the cues would end up in the video).
#pragma once
#include <functional>
#include <cstdint>
#include <string>
#include <vector>
#include "config.h"
#include "recorder.h"

struct CaptureSnapshot {
    bool           capable = false;     // a session with a working frame source exists in this process
    RecorderStatus rec;                 // state Idle when no recorder
    std::wstring   game, api, runtime, outputDir;
};

struct ControlHooks {
    std::function<CaptureSnapshot()> snapshot;  // called on the control thread
    std::function<void()> requestStart;         // the next xrEndFrame starts recording
    std::function<void()> requestStop;          // stops now (any thread)
};

void ControlStart(const Config& cfg, const std::wstring& logPath, const std::wstring& configPath, ControlHooks hooks);
void ControlStop();   // joins (bounded wait)
// config beeps (re-read at every recording start): the control thread copies the Config once at ControlStart, so a reload
// hands the new value over here. Any thread; logged when it changes.
void ControlSetBeeps(bool on);

// Publishes the result of a finalized recording ("saved <file>" / failure message, file, counters) to the tray's status
// block immediately and synchronously, from any thread -- the 250 ms publish loop may already be gone (game quit right after
// the recording ended). Keeps ownership / liveness untouched; ownerPid / state are only changed by the loop and at shutdown,
// message and file survive that. No-op when the control thread is not running.
void ControlPublishFinal(const RecorderStatus& rec, const std::wstring& outputDir);

// The WAV file (RIFF, PCM 16-bit mono 48 kHz, 5 ms fades) of cue `kind`: 0 start, 1 stop, 2 error. Pure; unit-tested.
std::vector<uint8_t> BuildCueWav(int kind);
