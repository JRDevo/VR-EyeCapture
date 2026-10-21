// capture_common -- the glue shared by the two capture front ends: the OpenXR layer (src/layer.cpp) and the SteamVR
// (OpenVR) proxy (src/openvr/). Everything here is API-independent: the XR_EYE_CAPTURE_* environment, the start-up
// config log, the recording file name, RecorderSettings from Config, the frame-rate pick, the per-recording config
// reload and the video clock. The front ends keep their own hooks, locks and per-session state.
#pragma once
#include <cstdint>
#include <string>
#include "config.h"
#include "frame_source.h"
#include "recorder.h"

// The XR_EYE_CAPTURE_* environment variables (test / automation hooks), read once at process start. Same names for the
// layer and the proxy, so every test setup applies to both.
struct CaptureEnv {
    std::wstring outputDir;          // XR_EYE_CAPTURE_OUTPUT_DIR (empty = not set)
    double       autoRecordSec = 0;  // XR_EYE_CAPTURE_AUTORECORD=<s>: record from the first frame for s seconds
    double       autoRecord2Sec = 0; // XR_EYE_CAPTURE_AUTORECORD2=<s>: a second autorecord ~1 s after the first stopped
    std::wstring configOverrides;    // XR_EYE_CAPTURE_CONFIG="key=value;key=value" (on top of config.ini)
    bool         trayIpc = true;     // XR_EYE_CAPTURE_TRAY_IPC=0: no control thread (no hotkey, nothing published)
    std::wstring requireTray;        // XR_EYE_CAPTURE_REQUIRE_TRAY (empty = not set; else overrides require_tray)
    std::wstring liveShmOverride;    // XR_EYE_CAPTURE_TEST_LIVE_SHM (empty = not set)
};
CaptureEnv ReadCaptureEnv();
std::wstring EnvW(const wchar_t* name);   // "" when unset or longer than 1023 characters

// Base name of the process exe without extension ("game" when unknown) and the folder of the module that contains
// `addressInModule` (no trailing slash; "." when unknown).
std::wstring ExeBaseName();
std::wstring ModuleDirOf(const void* addressInModule);

// The start-up lines every front end logs after its banner: "<folderWhat> <folder> | config ... | output ... | hotkey ...",
// head smoothing, symmetric_fov and dome.
void LogCaptureConfig(const char* folderWhat, const std::wstring& folder, const Config& c);

// Nearest common VR refresh rate (45 / 60 / 72 / 80 / 90 / 120 / 144 within 3 %), else the rounded rate.
unsigned SnapRate(double hz);
// Frame rate of a new recording: the measured submit rate after 30 samples, else refreshHz (> 1; `refreshName` names the
// source in *from), else config fps.
unsigned PickFps(const Config& c, double periodNs, int periodSamples, float refreshHz, const char* refreshName,
                 const char** from);

// Submitted-frame period estimate (EMA over the deltas of a monotonic frame time in ns; deltas outside 2..100 ms ignored).
struct PeriodEstimator {
    int64_t last = 0;
    double  periodNs = 0;
    int     samples = 0;
    void Add(int64_t tNs);
};

// <exe>_<yyyymmdd>_<hhmmss>_SBS.mp4 (flat), _VR180.mp4 / _VR360.mp4 (dome); local time now. Never a name that was
// already used: when this process handed out the same name before (two recordings within one second) or `dir` (if not
// empty) already has a file of that name or one derived from it (<stem>.mkv / .json / <stem>_FLAT.mp4 ...), a counter goes
// in front of the tag: <exe>_<stamp>_2_SBS.mp4, _3_ ... (the tag stays last: the tray and the docs rely on it).
std::wstring RecordingFileName(const std::wstring& exeName, DomeProjection p, const std::wstring& dir = std::wstring());
// Every RecorderSettings field that comes from the config and the two start views (path is left to the caller).
void FillRecorderSettings(const Config& c, const EyeView& L, const EyeView& R, unsigned fps, RecorderSettings& out);

// The video clock of one recording: maps the runtime's display time (ns) onto QPC (100 ns), anchored at the first frame;
// re-anchors (logged, at most 5 lines) when they drift more than 0.5 s apart.
struct VideoClockAnchor {
    bool    anchored = false;
    int64_t anchorQpc = 0;
    int64_t anchorDisp = 0;
    int     reanchors = 0;
    void    Reset() { anchored = false; reanchors = 0; }
    int64_t Map(int64_t displayTimeNs);
};

// Per-recording config reload (the tray's Settings window edits config.ini): copies the keys that can change per
// recording from `fresh` into `cur` and returns the "k=old->new, ..." text ("" = no changes). hotkey, require_tray,
// vr_indicator / indicator_*, live_share, [steamvr] and post_* keep their values. *outputChanged = output_dir changed.
std::string ApplyReloadedConfig(Config& cur, const Config& fresh, bool* outputChanged);
// The whole reload as both front ends do it at a recording start: LoadConfig(home) + the XR_EYE_CAPTURE_OUTPUT_DIR /
// _CONFIG overrides (read now), ApplyReloadedConfig, beeps to the control thread, log level / mirror, one log line.
// An unreadable config.ini keeps the current settings (warning).
void ReloadConfigForRecording(const std::wstring& home, Config& cur);
