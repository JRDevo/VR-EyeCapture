// config -- see config.h.
#include "config.h"
#include <windows.h>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool ParseUInt(const std::wstring& v, unsigned& out) {
    if (v.empty()) return false;
    unsigned long long x = 0;
    for (wchar_t c : v) {
        if (c < L'0' || c > L'9') return false;
        x = x * 10 + (unsigned)(c - L'0');
        if (x > 1000000000ull) return false;
    }
    out = (unsigned)x;
    return true;
}
bool ParseFloat(const std::wstring& v, float& out) {
    if (v.empty()) return false;
    wchar_t* end = nullptr;
    const double d = wcstod(v.c_str(), &end);
    if (!end || *end != L'\0' || !(d == d)) return false;
    out = (float)d;
    return true;
}
bool ParseBool(const std::wstring& v, bool& out) {
    const std::wstring l = Lower(v);
    if (l == L"1" || l == L"true" || l == L"yes" || l == L"on") { out = true; return true; }
    if (l == L"0" || l == L"false" || l == L"no" || l == L"off") { out = false; return true; }
    return false;
}

} // namespace

namespace {
enum class KeyResult { Ok, Bad, Unknown };
KeyResult ApplyKey(Config& c, const std::wstring& key, std::wstring val);
} // namespace

namespace {
// Shared by LoadConfig (problems == nullptr: log only) and ParseConfigFile (problems collected as well as logged).
void ParseFileInto(const std::wstring& path, Config& c, std::vector<std::wstring>* problems) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    std::wstringstream ws(elog::Widen(bytes));

    std::wstring line;
    int lineNo = 0;
    while (std::getline(ws, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#' || line[0] == L'[') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) { LOGW("config line %d: no '=' (ignored)", lineNo); continue; }
        const std::wstring key = Lower(Trim(line.substr(0, eq)));
        std::wstring val = Trim(line.substr(eq + 1));
        for (const wchar_t* cm : {L" ;", L"\t;", L" #", L"\t#"}) {      // trailing comments
            const size_t p = val.find(cm);
            if (p != std::wstring::npos) val = Trim(val.substr(0, p));
        }
        if (val.size() >= 2 && val.front() == L'"' && val.back() == L'"') val = val.substr(1, val.size() - 2);
        const std::string k8 = elog::Narrow(key), v8 = elog::Narrow(val);
        const KeyResult r = ApplyKey(c, key, val);
        if (r == KeyResult::Unknown) LOGW("config line %d: unknown key '%s' (ignored)", lineNo, k8.c_str());
        else if (r == KeyResult::Bad) {
            LOGW("config line %d: bad value '%s' for '%s' (default kept)", lineNo, v8.c_str(), k8.c_str());
            if (problems) problems->push_back(L"line " + std::to_wstring(lineNo) + L": bad value '" + val + L"' for " + key);
        }
    }
}
} // namespace

const char* SteamVrModeKey(SteamVrMode m) {
    switch (m) {
    case SteamVrMode::Quick: return "quick";
    case SteamVrMode::Off: return "off";
    default: return "per_game";
    }
}

const char* SteamVrPoseSourceKey(SteamVrPoseSource s) {
    switch (s) {
    case SteamVrPoseSource::Submit: return "submit";
    case SteamVrPoseSource::WaitGetPoses: return "waitgetposes";
    default: return "auto";
    }
}

namespace {
// %APPDATA%\<folder>\config.ini, or "" when APPDATA is not set.
std::wstring AppDataConfig(const wchar_t* folder) {
    wchar_t appdata[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(appdata) + L"\\" + folder + L"\\config.ini";
}
} // namespace

std::wstring ConfigPath(const std::wstring& dllDir) {
    for (const wchar_t* folder : {kAppDataFolder, kOldAppDataFolder}) {
        const std::wstring p = AppDataConfig(folder);
        if (!p.empty() && FileExists(p)) return p;
    }
    return dllDir + L"\\config.ini";
}

int MigrateAppDataConfig(std::wstring* from, std::wstring* to) {
    const std::wstring oldP = AppDataConfig(kOldAppDataFolder), newP = AppDataConfig(kAppDataFolder);
    if (from) *from = oldP;
    if (to) *to = newP;
    if (oldP.empty() || newP.empty() || FileExists(newP) || !FileExists(oldP)) return 0;
    CreateDirectoryW(newP.substr(0, newP.find_last_of(L'\\')).c_str(), nullptr);
    if (!CopyFileW(oldP.c_str(), newP.c_str(), TRUE)) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_EXISTS ? 0 : -(int)(e ? e : 1);
    }
    return 1;
}

void LoadConfig(const std::wstring& dllDir, Config& c) {
    c = Config();
    ParseHotkey(kDefaultHotkey, c.hotkey);
    c.outputDir = dllDir + L"\\captures";

    const std::wstring path = ConfigPath(dllDir);
    if (!FileExists(path)) {
        LOGW("config: no config.ini found (looked in %%APPDATA%%\\VR EyeCapture, %%APPDATA%%\\OpenXR-EyeCapture and %s); using defaults",
             elog::Narrow(dllDir).c_str());
        return;
    }
    c.loadedFrom = path;

    ParseFileInto(path, c, nullptr);
}

bool ParseConfigFile(const std::wstring& path, Config& out, std::vector<std::wstring>& problems) {
    out = Config();
    ParseHotkey(kDefaultHotkey, out.hotkey);
    problems.clear();
    if (!FileExists(path)) { problems.push_back(L"cannot read " + path); return false; }
    out.loadedFrom = path;
    ParseFileInto(path, out, &problems);
    return problems.empty();
}

void ApplyConfigOverrides(const std::wstring& list, Config& c) {
    std::wstring rest = list;
    while (!rest.empty()) {
        const size_t semi = rest.find(L';');
        const std::wstring item = Trim(rest.substr(0, semi));
        rest = semi == std::wstring::npos ? std::wstring() : rest.substr(semi + 1);
        if (item.empty()) continue;
        const size_t eq = item.find(L'=');
        const std::wstring key = Lower(Trim(item.substr(0, eq)));
        const std::wstring val = eq == std::wstring::npos ? std::wstring() : Trim(item.substr(eq + 1));
        const KeyResult r = eq == std::wstring::npos ? KeyResult::Bad : ApplyKey(c, key, val);
        if (r == KeyResult::Ok) LOGI("config override: %s = %s", elog::Narrow(key).c_str(), elog::Narrow(val).c_str());
        else LOGW("config override '%s' ignored (%s)", elog::Narrow(item).c_str(), r == KeyResult::Unknown ? "unknown key" : "bad value");
    }
}

namespace {
KeyResult ApplyKey(Config& c, const std::wstring& key, std::wstring val) {
    bool ok = true;
    unsigned u = 0;
    bool b = false;
    float fl = 0;
    if (key == L"hotkey") {
        Hotkey h;
        if (ParseHotkey(val, h)) c.hotkey = h; else ok = false;
    } else if (key == L"output_dir") {
        while (!val.empty() && (val.back() == L'\\' || val.back() == L'/')) val.pop_back();
        if (!val.empty()) c.outputDir = val;
    } else if (key == L"codec") {
        const std::wstring l = Lower(val);
        if (l == L"hevc" || l == L"h265" || l == L"h.265") c.codec = VideoCodec::Hevc;
        else if (l == L"h264" || l == L"h.264" || l == L"avc") c.codec = VideoCodec::H264;
        else if (l == L"av1") c.codec = VideoCodec::Av1;
        else ok = false;
    } else if (key == L"bitrate_mbps") {
        if (ParseUInt(val, u) && u <= 1000) c.bitrateMbps = u; else ok = false;
    } else if (key == L"fps") {
        if (ParseUInt(val, u) && u >= 10 && u <= 240) c.fps = u; else ok = false;
    } else if (key == L"max_fps") {
        if (ParseUInt(val, u) && u <= 240) c.maxFps = u; else ok = false;
    } else if (key == L"rate_control") {
        const std::wstring l = Lower(val);
        if (l == L"vbr") c.rateControl = RateControl::Vbr;
        else if (l == L"cbr") c.rateControl = RateControl::Cbr;
        else if (l == L"qp" || l == L"cqp") c.rateControl = RateControl::Qp;
        else ok = false;
    } else if (key == L"qp") {
        if (ParseUInt(val, u) && u <= 51) c.qp = u; else ok = false;
    } else if (key == L"quality_vs_speed") {
        if (Lower(val) == L"auto") c.qualityVsSpeed = -1;
        else if (ParseUInt(val, u) && u <= 100) c.qualityVsSpeed = (int)u;
        else ok = false;
    } else if (key == L"constant_frame_rate") {
        if (ParseBool(val, b)) c.constantFrameRate = b; else ok = false;
    } else if (key == L"beeps") {
        if (ParseBool(val, b)) c.beeps = b; else ok = false;
    } else if (key == L"max_width") {
        if (ParseUInt(val, u) && (u == 0 || (u >= 256 && u <= 16384))) c.maxWidth = u & ~1u; else ok = false;
    } else if (key == L"audio_enabled") {
        if (ParseBool(val, b)) c.audioEnabled = b; else ok = false;
    } else if (key == L"audio_device") {
        c.audioDevice = val;
    } else if (key == L"gamma") {
        const std::wstring l = Lower(val);
        if (l == L"auto") c.gamma = GammaMode::Auto;
        else if (l == L"srgb") c.gamma = GammaMode::Srgb;
        else if (l == L"linear") c.gamma = GammaMode::Linear;
        else ok = false;
    } else if (key == L"require_tray") {
        if (ParseBool(val, b)) c.requireTray = b; else ok = false;
    } else if (key == L"swap_eyes") {
        if (ParseBool(val, b)) c.swapEyes = b; else ok = false;
    } else if (key == L"record_overlays") {
        if (ParseBool(val, b)) c.recordOverlays = b; else ok = false;
    } else if (key == L"live_share") {
        if (ParseBool(val, b)) c.liveShare = b; else ok = false;
    } else if (key == L"post_mkv") {
        if (ParseBool(val, b)) c.postMkv = b; else ok = false;
    } else if (key == L"keep_mp4") {
        if (ParseBool(val, b)) c.keepMp4 = b; else ok = false;
    } else if (key == L"post_flat") {
        if (ParseBool(val, b)) c.postFlat = b; else ok = false;
    } else if (key == L"record_for_youtube") {
        if (ParseBool(val, b)) c.recordForYoutube = b; else ok = false;
    } else if (key == L"yt8k_bitrate_mbps") {
        if (ParseUInt(val, u) && u >= 20 && u <= 400) c.yt8kBitrateMbps = u; else ok = false;
    } else if (key == L"yt8k_codec") {
        const std::wstring l = Lower(val);
        if (l == L"hevc" || l == L"h265" || l == L"h.265") c.yt8kCodec = VideoCodec::Hevc;
        else if (l == L"av1") c.yt8kCodec = VideoCodec::Av1;
        else ok = false;
    } else if (key == L"post_yt8k") {
        if (ParseBool(val, b)) c.postYt8k = b; else ok = false;
    } else if (key == L"post_when") {
        const std::wstring l = Lower(val);
        if (l == L"now") c.postWhen = PostWhen::Now;
        else if (l == L"session_end") c.postWhen = PostWhen::SessionEnd;
        else if (l == L"game_exit") c.postWhen = PostWhen::GameExit;
        else if (l == L"manual") c.postWhen = PostWhen::Manual;
        else ok = false;
    } else if (key == L"layout_3d") {
        const std::wstring l = Lower(val);
        if (l == L"tb_half") c.layout3d = Layout3d::TbHalf;
        else if (l == L"sbs_half") c.layout3d = Layout3d::SbsHalf;
        else if (l == L"sbs_full") c.layout3d = Layout3d::SbsFull;
        else if (l == L"tb_full") c.layout3d = Layout3d::TbFull;
        else { c.layout3d = Layout3d::TbHalf; ok = false; }   // unknown: tb_half (the bad-value message names the key)
    } else if (key == L"eye_crop") {
        const std::wstring l = Lower(val);
        if (l == L"16:9") c.eyeCrop = EyeCrop::Wide169;
        else if (l == L"4:3") c.eyeCrop = EyeCrop::Std43;
        else if (l == L"4:3pad") c.eyeCrop = EyeCrop::Pad43;
        else if (l == L"none") c.eyeCrop = EyeCrop::None;
        else { c.eyeCrop = EyeCrop::Wide169; ok = false; }   // unknown: 16:9 (the bad-value message names the key)
    } else if (key == L"ffmpeg_path") {
        c.ffmpegPath = val;
    } else if (key == L"head_smoothing") {
        if (ParseFloat(val, fl) && fl >= 0.0f && fl <= 1.0f) c.head.smoothing = fl; else ok = false;
    } else if (key == L"head_follow_deg") {
        if (ParseFloat(val, fl) && fl >= 1.0f && fl <= 90.0f) c.head.followDeg = fl; else ok = false;
    } else if (key == L"roll_lock") {
        if (ParseBool(val, b)) c.head.rollLock = b; else ok = false;
    } else if (key == L"output_fov_scale") {
        if (ParseFloat(val, fl) && fl >= 0.5f && fl <= 1.0f) c.head.fovScale = fl; else ok = false;
    } else if (key == L"edge_guard") {
        if (ParseBool(val, b)) c.head.edgeGuard = b; else ok = false;
    } else if (key == L"vr_indicator") {
        if (ParseBool(val, b)) c.indicator.enabled = b; else ok = false;
    } else if (key == L"indicator_x") {
        if (ParseFloat(val, fl) && fl >= -2.0f && fl <= 2.0f) c.indicator.x = fl; else ok = false;
    } else if (key == L"indicator_y") {
        if (ParseFloat(val, fl) && fl >= -2.0f && fl <= 2.0f) c.indicator.y = fl; else ok = false;
    } else if (key == L"indicator_z") {
        if (ParseFloat(val, fl) && fl >= -10.0f && fl <= -0.05f) c.indicator.z = fl; else ok = false;
    } else if (key == L"indicator_size") {
        if (ParseFloat(val, fl) && fl >= 0.0005f && fl <= 0.5f) c.indicator.size = fl; else ok = false;
    } else if (key == L"indicator_opacity") {
        if (ParseFloat(val, fl) && fl >= 0.0f && fl <= 1.0f) c.indicator.opacity = fl; else ok = false;
    } else if (key == L"indicator_live") {
        if (ParseBool(val, b)) c.indicator.live = b; else ok = false;
    } else if (key == L"symmetric_fov") {
        if (ParseBool(val, b)) c.head.symmetricFov = b; else ok = false;
    } else if (key == L"projection") {
        const std::wstring l = Lower(val);
        if (l == L"flat") c.dome.projection = DomeProjection::Flat;
        else if (l == L"vr180") c.dome.projection = DomeProjection::Vr180;
        else if (l == L"vr360") c.dome.projection = DomeProjection::Vr360;
        else ok = false;
    } else if (key == L"dome_width") {
        if (ParseUInt(val, u) && (u == 0 || (u >= 256 && u <= 16384))) c.dome.width = u; else ok = false;
    } else if (key == L"dome_forward") {
        const std::wstring l = Lower(val);
        if (l == L"reset_on_hotkey") LOGW("config: dome_forward = reset_on_hotkey is not implemented yet -- using start");
        else if (l != L"start") ok = false;
    } else if (key == L"fill") {
        const std::wstring l = Lower(val);
        if (l == L"black") c.dome.fill = DomeFill::Black;
        else if (l == L"last_seen") c.dome.fill = DomeFill::LastSeen;
        else ok = false;
    } else if (key == L"fill_fade_s") {
        if (ParseFloat(val, fl) && fl >= 0.0f && fl <= 3600.0f) c.dome.fadeS = fl; else ok = false;
    } else if (key == L"window_feather_deg") {
        if (ParseFloat(val, fl) && fl >= 0.0f && fl <= 20.0f) c.dome.featherDeg = fl; else ok = false;
    } else if (key == L"steamvr_enabled") {
        if (ParseBool(val, b)) c.steamvr.enabled = b; else ok = false;
    } else if (key == L"steamvr_mode") {
        const std::wstring l = Lower(val);
        if (l == L"per_game" || l == L"pergame") c.steamvr.mode = SteamVrMode::PerGame;
        else if (l == L"quick") c.steamvr.mode = SteamVrMode::Quick;
        else if (l == L"off") c.steamvr.mode = SteamVrMode::Off;
        else ok = false;
    } else if (key == L"steamvr_indicator") {
        if (ParseBool(val, b)) c.steamvr.indicator = b; else ok = false;
    } else if (key == L"steamvr_pose_source") {
        const std::wstring l = Lower(val);
        if (l == L"auto") c.steamvr.poseSource = SteamVrPoseSource::Auto;
        else if (l == L"submit") c.steamvr.poseSource = SteamVrPoseSource::Submit;
        else if (l == L"waitgetposes") c.steamvr.poseSource = SteamVrPoseSource::WaitGetPoses;
        else ok = false;
    } else if (key == L"log_level") {
        const std::wstring l = Lower(val);
        if (l == L"none" || l == L"off") c.logLevel = LogLevel::None;
        else if (l == L"error") c.logLevel = LogLevel::Error;
        else if (l == L"warn" || l == L"warning") c.logLevel = LogLevel::Warn;
        else if (l == L"info") c.logLevel = LogLevel::Info;
        else if (l == L"debug") c.logLevel = LogLevel::Debug;
        else ok = false;
    } else {
        return KeyResult::Unknown;
    }
    return ok ? KeyResult::Ok : KeyResult::Bad;
}
} // namespace
