// control -- the layer's control thread: hotkey polling (GetAsyncKeyState, works while the game has focus), commands
// from EyeCaptureTray.exe through shared memory, status publishing for the tray, and the audible cues.
//
// Cues (Beep goes to the default audio device, so a headset on Virtual Desktop / Link hears it):
//   start OK       1 tone  880 Hz
//   stop           2 tones 660 Hz
//   failed / none  1 long low tone 220 Hz (no capture-capable session, start failed, or no frames for 6 s)
#pragma once
#include <functional>
#include <string>
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
