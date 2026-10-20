// capture_common -- see capture_common.h.
#include "capture_common.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "control.h"
#include "log.h"
#include "yt_layout.h"

std::wstring EnvW(const wchar_t* name) {
    wchar_t b[1024];
    const DWORD n = GetEnvironmentVariableW(name, b, 1024);
    return (n > 0 && n < 1024) ? std::wstring(b) : std::wstring();
}

CaptureEnv ReadCaptureEnv() {
    CaptureEnv e;
    e.outputDir = EnvW(L"XR_EYE_CAPTURE_OUTPUT_DIR");
    const std::wstring autoRec = EnvW(L"XR_EYE_CAPTURE_AUTORECORD");
    if (!autoRec.empty()) e.autoRecordSec = _wtof(autoRec.c_str());
    const std::wstring autoRec2 = EnvW(L"XR_EYE_CAPTURE_AUTORECORD2");
    if (!autoRec2.empty()) e.autoRecord2Sec = _wtof(autoRec2.c_str());
    e.configOverrides = EnvW(L"XR_EYE_CAPTURE_CONFIG");
    e.trayIpc = EnvW(L"XR_EYE_CAPTURE_TRAY_IPC") != L"0";
    e.requireTray = EnvW(L"XR_EYE_CAPTURE_REQUIRE_TRAY");
    e.liveShmOverride = EnvW(L"XR_EYE_CAPTURE_TEST_LIVE_SHM");
    return e;
}

std::wstring ExeBaseName() {
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    const size_t k = s.find_last_of(L"\\/");
    if (k != std::wstring::npos) s = s.substr(k + 1);
    const size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos) s = s.substr(0, dot);
    return s.empty() ? L"game" : s;
}

std::wstring ModuleDirOf(const void* addressInModule) {
    HMODULE hm = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)addressInModule, &hm);
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(hm, p, MAX_PATH);
    std::wstring s = p;
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

void LogCaptureConfig(const char* folderWhat, const std::wstring& folder, const Config& c) {
    char qvsText[16];
    if (c.qualityVsSpeed >= 0) snprintf(qvsText, sizeof(qvsText), "%d", c.qualityVsSpeed);
    else snprintf(qvsText, sizeof(qvsText), "auto");
    LOGI("%s %s | config %s | output %s | hotkey %s | codec %s %u Mb/s | rate_control %s | qp %u | "
         "quality_vs_speed %s | max_width %u | max_fps %u | constant_frame_rate %d | audio %s%s%s | gamma %s | beeps %d",
         folderWhat, elog::Narrow(folder).c_str(), c.loadedFrom.empty() ? "(defaults)" : elog::Narrow(c.loadedFrom).c_str(),
         elog::Narrow(c.outputDir).c_str(), elog::Narrow(c.hotkey.text).c_str(), VideoCodecName(c.codec),
         c.bitrateMbps ? c.bitrateMbps : VideoCodecDefaultMbps(c.codec), RateControlKey(c.rateControl), c.qp,
         qvsText, c.maxWidth, c.maxFps, c.constantFrameRate ? 1 : 0,
         c.audioEnabled ? "on" : "off", c.audioDevice.empty() ? "" : " device~",
         elog::Narrow(c.audioDevice).c_str(),
         c.gamma == GammaMode::Auto ? "auto" : c.gamma == GammaMode::Srgb ? "srgb" : "linear", c.beeps ? 1 : 0);
    if (c.head.Enabled())
        LOGI("head smoothing: head_smoothing %.2f (time constant %.3f s) | head_follow_deg %.1f | roll_lock %d | "
             "output_fov_scale %.2f | edge_guard %d",
             c.head.smoothing, HeadSmoothingTimeConstant(c.head.smoothing), c.head.followDeg,
             c.head.rollLock ? 1 : 0, c.head.fovScale, c.head.edgeGuard ? 1 : 0);
    else
        LOGI("head smoothing: off (head_smoothing = 0)");
    LOGI("symmetric_fov %d: %s", c.head.symmetricFov ? 1 : 0,
         c.head.symmetricFov ? "both eyes re-projected to one common symmetric view (standard 3D SBS)"
                             : "raw eye images (asymmetric per-eye fov)");
    if (c.dome.Enabled())
        LOGI("dome: projection %s (%s, files *_%s.mp4) | dome_width %u%s | fill %s | fill_fade_s %.1f | window_feather_deg %.1f | "
             "dome_forward start -- symmetric_fov / head smoothing are ignored",
             DomeProjectionName(c.dome.projection),
             c.dome.projection == DomeProjection::Vr180 ? "VR180, left | right" : "VR360, left on top / right below",
             DomeFileTag(c.dome.projection), c.dome.width, c.dome.width ? "" : " (from the source density)",
             c.dome.fill == DomeFill::LastSeen ? "last_seen" : "black", c.dome.fadeS, c.dome.featherDeg);
    else
        LOGI("dome: projection flat (standard SBS; [dome] projection = vr180 | vr360 for a world-locked dome)");
}

unsigned SnapRate(double hz) {
    const unsigned common[] = {45, 60, 72, 80, 90, 120, 144};
    for (unsigned c : common) if (std::fabs(hz - c) <= c * 0.03) return c;
    return (unsigned)std::lround(hz);
}

unsigned PickFps(const Config& c, double periodNs, int periodSamples, float refreshHz, const char* refreshName,
                 const char** from) {
    unsigned fps = 0;
    const char* src = "config";
    if (periodSamples >= 30 && periodNs > 0) { fps = SnapRate(1e9 / periodNs); src = "measured frame submit rate"; }
    else if (refreshHz > 1) { fps = SnapRate(refreshHz); src = refreshName; }
    if (!fps) fps = c.fps;
    if (from) *from = src;
    return fps;
}

void PeriodEstimator::Add(int64_t tNs) {
    if (last && tNs > last) {
        const double d = (double)(tNs - last);
        if (d > 2e6 && d < 1e8) {
            periodNs = samples ? periodNs * 0.95 + d * 0.05 : d;
            ++samples;
        }
    }
    last = tNs;
}

std::wstring RecordingFileName(const std::wstring& exeName, DomeProjection p) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    // <exe>_<stamp>_SBS.mp4 (flat), _VR180.mp4 / _VR360.mp4 (dome)
    swprintf_s(name, L"%s_%04u%02u%02u_%02u%02u%02u_%hs.mp4", exeName.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, DomeFileTag(p));
    return name;
}

void FillRecorderSettings(const Config& c, const EyeView& L, const EyeView& R, unsigned fps, RecorderSettings& rs) {
    rs.codec = c.codec;
    rs.bitrate = c.bitrateMbps * 1000000u;
    rs.rateControl = c.rateControl;
    rs.qp = c.qp;
    rs.qualityVsSpeed = c.qualityVsSpeed;
    rs.constantFrameRate = c.constantFrameRate;
    rs.fps = fps;
    rs.maxFps = c.maxFps;
    rs.maxWidth = c.maxWidth;
    rs.audio = c.audioEnabled;
    rs.audioDevice = c.audioDevice;
    rs.head = c.head;
    rs.dome = c.dome;
    rs.youtube = c.recordForYoutube;   // record_for_youtube: render the YouTube layout (layout_3d, eye_crop) directly
    rs.tag3d = c.tag3d;
    rs.layout3d = c.layout3d;
    rs.eyeCrop = c.eyeCrop;
    rs.startFovValid = L.hasPose && R.hasPose;
    for (int i = 0; i < 4; ++i) { rs.startFov[0][i] = L.fov[i]; rs.startFov[1][i] = R.fov[i]; }
    rs.startEyeW[0] = L.w; rs.startEyeH[0] = L.h; rs.startEyeW[1] = R.w; rs.startEyeH[1] = R.h;
}

int64_t VideoClockAnchor::Map(int64_t displayTime) {
    const int64_t now = QpcNow100ns();
    if (!anchored) {
        anchored = true;
        anchorQpc = now;
        anchorDisp = displayTime;
    }
    int64_t ts = anchorQpc + (displayTime - anchorDisp) / 100;
    if (ts - now > 5000000 || now - ts > 5000000) {   // runtime clock jumped (pause / resync): re-anchor on QPC
        if (++reanchors <= 5) LOGW("record: displayTime drifted %.1f ms from QPC -- re-anchoring", (ts - now) / 1e4);
        anchorQpc = now;
        anchorDisp = displayTime;
        ts = now;
    }
    return ts;
}

std::string ApplyReloadedConfig(Config& c, const Config& n, bool* outputChanged) {
    std::string changes;
    auto note = [&](const char* key, const std::string& o, const std::string& v) {
        if (o == v) return;
        if (!changes.empty()) changes += ", ";
        changes += std::string(key) + "=" + o + "->" + v;
    };
    auto B = [](bool b) { return std::string(b ? "1" : "0"); };
    auto U = [](unsigned u) { return std::to_string(u); };
    auto F = [](float f) { char b[32]; snprintf(b, sizeof(b), "%g", f); return std::string(b); };
    note("output_dir", elog::Narrow(c.outputDir), elog::Narrow(n.outputDir));
    note("codec", VideoCodecKey(c.codec), VideoCodecKey(n.codec));
    auto Q = [](int q) { return q < 0 ? std::string("auto") : std::to_string(q); };
    note("bitrate_mbps", U(c.bitrateMbps), U(n.bitrateMbps));
    note("rate_control", RateControlKey(c.rateControl), RateControlKey(n.rateControl));
    note("qp", U(c.qp), U(n.qp));
    note("quality_vs_speed", Q(c.qualityVsSpeed), Q(n.qualityVsSpeed));
    note("fps", U(c.fps), U(n.fps));
    note("max_fps", U(c.maxFps), U(n.maxFps));
    note("constant_frame_rate", B(c.constantFrameRate), B(n.constantFrameRate));
    note("max_width", U(c.maxWidth), U(n.maxWidth));
    note("record_for_youtube", B(c.recordForYoutube), B(n.recordForYoutube));
    note("tag_3d", B(c.tag3d), B(n.tag3d));
    note("layout_3d", YtPackingKey(c.layout3d), YtPackingKey(n.layout3d));
    note("eye_crop", YtCropKey(c.eyeCrop), YtCropKey(n.eyeCrop));
    note("gamma", c.gamma == GammaMode::Auto ? "auto" : c.gamma == GammaMode::Srgb ? "srgb" : "linear",
         n.gamma == GammaMode::Auto ? "auto" : n.gamma == GammaMode::Srgb ? "srgb" : "linear");
    note("swap_eyes", B(c.swapEyes), B(n.swapEyes));
    note("record_overlays", B(c.recordOverlays), B(n.recordOverlays));
    note("audio_enabled", B(c.audioEnabled), B(n.audioEnabled));
    note("audio_device", elog::Narrow(c.audioDevice), elog::Narrow(n.audioDevice));
    note("beeps", B(c.beeps), B(n.beeps));
    note("symmetric_fov", B(c.head.symmetricFov), B(n.head.symmetricFov));
    note("head_smoothing", F(c.head.smoothing), F(n.head.smoothing));
    note("head_follow_deg", F(c.head.followDeg), F(n.head.followDeg));
    note("roll_lock", B(c.head.rollLock), B(n.head.rollLock));
    note("output_fov_scale", F(c.head.fovScale), F(n.head.fovScale));
    note("edge_guard", B(c.head.edgeGuard), B(n.head.edgeGuard));
    note("projection", DomeProjectionName(c.dome.projection), DomeProjectionName(n.dome.projection));
    note("dome_width", U(c.dome.width), U(n.dome.width));
    note("fill", c.dome.fill == DomeFill::LastSeen ? "last_seen" : "black", n.dome.fill == DomeFill::LastSeen ? "last_seen" : "black");
    note("fill_fade_s", F(c.dome.fadeS), F(n.dome.fadeS));
    note("window_feather_deg", F(c.dome.featherDeg), F(n.dome.featherDeg));
    static const char* kLevel[] = {"none", "error", "warn", "info", "debug"};   // index = (int)LogLevel + 1
    auto lv = [&](LogLevel l) { return std::string(kLevel[(int)l >= -1 && (int)l < 4 ? (int)l + 1 : 3]); };
    note("log_level", lv(c.logLevel), lv(n.logLevel));

    if (outputChanged) *outputChanged = c.outputDir != n.outputDir;
    c.outputDir = n.outputDir; c.codec = n.codec; c.bitrateMbps = n.bitrateMbps; c.fps = n.fps; c.maxFps = n.maxFps;
    c.rateControl = n.rateControl; c.qp = n.qp; c.qualityVsSpeed = n.qualityVsSpeed; c.constantFrameRate = n.constantFrameRate;
    c.beeps = n.beeps;
    c.maxWidth = n.maxWidth; c.gamma = n.gamma; c.swapEyes = n.swapEyes; c.recordOverlays = n.recordOverlays;
    c.tag3d = n.tag3d;
    c.recordForYoutube = n.recordForYoutube; c.layout3d = n.layout3d; c.eyeCrop = n.eyeCrop;   // record_for_youtube layout
    c.audioEnabled = n.audioEnabled;
    c.audioDevice = n.audioDevice; c.head = n.head; c.dome = n.dome; c.logLevel = n.logLevel;
    return changes;
}

// Called with the front end's state lock held, on the game thread, before the swap_eyes decision.
void ReloadConfigForRecording(const std::wstring& home, Config& c) {
    Config n;
    LoadConfig(home, n);
    if (n.loadedFrom.empty()) {
        LOGW("config reload: %s could not be read -- keeping the current settings", elog::Narrow(ConfigPath(home)).c_str());
        return;
    }
    const std::wstring outOverride = EnvW(L"XR_EYE_CAPTURE_OUTPUT_DIR");
    if (!outOverride.empty()) n.outputDir = outOverride;
    const std::wstring cfgOverride = EnvW(L"XR_EYE_CAPTURE_CONFIG");
    if (!cfgOverride.empty()) ApplyConfigOverrides(cfgOverride, n);

    bool outChanged = false;
    const std::string changes = ApplyReloadedConfig(c, n, &outChanged);
    ControlSetBeeps(c.beeps);   // the control thread copied the Config at its start; the start / stop cues follow this
    elog::SetLevel(c.logLevel);
    if (outChanged) elog::SetMirror(c.outputDir + L"\\eye_capture.log");
    LOGI("config reloaded for this recording from %s: %s%s", elog::Narrow(n.loadedFrom).c_str(),
         changes.empty() ? "no changes" : "changed ", changes.c_str());
}
