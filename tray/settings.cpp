// settings -- see settings.h.
#include "settings.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "input.h"
#include "disk.h"
#include "glass.h"
#include "install.h"
#include "log.h"
#include "postprocess.h"
#pragma warning(push)
#pragma warning(disable : 4458)   // gdiplus headers: declaration hides class member
#include <objidl.h>
#include <gdiplus.h>
#pragma warning(pop)

#pragma comment(lib, "shcore.lib")   // GetDpiForMonitor
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace settings {

const wchar_t* const kWindowTitle = L"VR EyeCapture";

namespace {

enum class Kind { Edit, Combo, Check, Track };
struct Field {
    const wchar_t* key;
    const wchar_t* section;
    Kind           kind;
    int            id;
    const wchar_t* options;   // combo: '|'-separated config values
    const wchar_t* labels = nullptr;   // combo: '|'-separated display texts, same order as options (nullptr = show the values)
    const wchar_t* custom = nullptr;   // combo: format of an extra item for a file value that is none of the options
                                       // (%s = the value; it is kept on Save unless another item is picked); nullptr = none
};
const Field kFields[] = {
    {L"hotkey",            L"capture",         Kind::Edit,  ID_HOTKEY,     nullptr},
    {L"vr_hotkey",         L"capture",         Kind::Combo, ID_VRHOTKEY,   L"thumbsticks|left_x+right_a|left_y+right_b|left_menu+right_thumbstick|off",
     L"Both stick clicks|X + A buttons|Y + B buttons|Menu + right stick click|Off", L"Custom (%s)"},
    {L"output_dir",        L"capture",         Kind::Edit,  ID_OUTDIR,     nullptr},
    {L"codec",             L"capture",         Kind::Combo, ID_CODEC,      L"hevc|h264|av1",
     L"H.265 / HEVC (recommended)|H.264 / AVC|AV1 (hardware: RTX 40 / 50)"},
    {L"bitrate_mbps",      L"capture",         Kind::Combo, ID_BITRATE,    L"0|150|200|250|300|350|400|450|500",
     L"Default (120 Mbit/s)|150 Mbit/s|200 Mbit/s|250 Mbit/s|300 Mbit/s|350 Mbit/s|400 Mbit/s|450 Mbit/s|500 Mbit/s", L"Custom (%s Mbit/s)"},
    {L"rate_control",      L"capture",         Kind::Combo, ID_RATECONTROL, L"vbr|cbr|qp",
     L"VBR (default)|CBR (constant bitrate)|Constant QP (quality)"},
    {L"qp",                L"capture",         Kind::Edit,  ID_QP,         nullptr},
    {L"quality_vs_speed",  L"capture",         Kind::Combo, ID_PRESET,     L"auto|0|25|50|75|100",
     L"Auto (default)|Fastest|Fast|Balanced|Quality|Best quality", L"Custom (%s)"},
    {L"bit_depth",         L"capture",         Kind::Combo, ID_BITDEPTH,   L"8|10", L"8-bit (default)|10-bit (HEVC / AV1)"},
    {L"sharpen",           L"capture",         Kind::Combo, ID_SHARPEN,    L"0|0.3|0.6|1",
     L"Off (default)|Light|Medium|Strong", L"Custom (%s)"},
    {L"max_fps",           L"capture",         Kind::Edit,  ID_MAXFPS,     nullptr},
    {L"constant_frame_rate", L"capture",       Kind::Check, ID_CFR,        nullptr},
    {L"beeps",             L"capture",         Kind::Check, ID_BEEPS,      nullptr},
    {L"max_width",         L"capture",         Kind::Combo, ID_MAXWIDTH,   L"0|3840|2560|1920",
     L"Native (full resolution)|3840 px (4K)|2560 px (2K)|1920 px (1080p)", L"Custom (%s px)"},
    {L"require_tray",      L"capture",         Kind::Check, ID_REQTRAY,    nullptr},
    {L"swap_eyes",         L"capture",         Kind::Check, ID_SWAP,       nullptr},
    {L"record_overlays",   L"capture",         Kind::Check, ID_OVERLAYS,   nullptr},
    {L"record_for_youtube", L"capture",        Kind::Check, ID_RECORDYT,   nullptr},
    {L"tag_3d",            L"capture",         Kind::Check, ID_TAG3DMP4,   nullptr},
    {L"symmetric_fov",     L"head_smoothing",  Kind::Check, ID_SYMFOV,     nullptr},
    {L"head_smoothing",    L"head_smoothing",  Kind::Track, ID_SMOOTH,     nullptr},
    {L"roll_lock",         L"head_smoothing",  Kind::Check, ID_ROLL,       nullptr},
    {L"output_fov_scale",  L"head_smoothing",  Kind::Edit,  ID_FOVSCALE,   nullptr},
    {L"projection",        L"dome",            Kind::Combo, ID_PROJECTION, L"flat|vr180|vr360", L"Flat stereo (side by side)|VR180 dome (experimental)|VR360 dome (experimental)"},
    {L"layout_3d",         L"dome",            Kind::Combo, ID_LAYOUT3D,   L"tb_half|sbs_half|sbs_full|tb_full",
     L"Top/bottom, half height (YouTube)|Side by side, half width|Side by side, full|Top/bottom, full"},
    {L"fill",              L"dome",            Kind::Combo, ID_FILL,       L"black|last_seen", L"Black|Hold last seen view"},
    {L"fill_fade_s",       L"dome",            Kind::Edit,  ID_FILLFADE,   nullptr},
    {L"dome_shared_view",  L"dome",            Kind::Check, ID_SHAREDVIEW, nullptr},
    {L"vr_indicator",      L"indicator",       Kind::Check, ID_INDICATOR,  nullptr},
    {L"indicator_x",       L"indicator",       Kind::Edit,  ID_INDX,       nullptr},
    {L"indicator_y",       L"indicator",       Kind::Edit,  ID_INDY,       nullptr},
    {L"indicator_z",       L"indicator",       Kind::Edit,  ID_INDZ,       nullptr},
    {L"indicator_size",    L"indicator",       Kind::Edit,  ID_INDSIZE,    nullptr},
    {L"indicator_opacity", L"indicator",       Kind::Edit,  ID_INDOPACITY, nullptr},
    {L"indicator_live",    L"indicator",       Kind::Check, ID_INDLIVE,    nullptr},
    {L"audio_enabled",     L"audio",           Kind::Check, ID_AUDIO,      nullptr},
    {L"steamvr_mode",      L"steamvr",         Kind::Combo, ID_SVRMODE,    L"per_game|quick|off",
     L"Per game (best quality)|Quick mode (no setup, lower quality)|Off (not recorded)"},
    {L"audio_device",      L"audio",           Kind::Edit,  ID_AUDIODEV,   nullptr},
    {L"post_mkv",          L"post_processing", Kind::Check, ID_POSTMKV,    nullptr},
    {L"keep_mp4",          L"post_processing", Kind::Check, ID_KEEPMP4,    nullptr},
    {L"post_flat",         L"post_processing", Kind::Check, ID_POSTFLAT,   nullptr},
    {L"post_yt8k",         L"post_processing", Kind::Check, ID_POSTYT8K,   nullptr},
    {L"yt8k_bitrate_mbps", L"post_processing", Kind::Combo, ID_YT8KBITRATE, L"100|150|200",
     L"100 Mbit/s|150 Mbit/s (default)|200 Mbit/s", L"Custom (%s Mbit/s)"},
    {L"yt8k_codec",        L"post_processing", Kind::Combo, ID_YT8KCODEC,  L"hevc|av1", L"HEVC (default)|AV1 (RTX 40 / 50)"},
    {L"post_when",         L"post_processing", Kind::Combo, ID_POSTWHEN,   L"now|session_end|game_exit|manual",
     L"Immediately|When the VR session ends|When the game exits|When I say"},
    {L"eye_crop",          L"post_processing", Kind::Combo, ID_EYECROP,    L"16:9|4:3|4:3pad|none",
     L"16:9 (recommended)|4:3|4:3, padded to 16:9 (8K tier, black side bars)|None (full eye)"},
    {L"log_level",         L"debug",           Kind::Combo, ID_LOGLEVEL,   L"none|error|warn|info|debug", L"Off|Errors|Warnings|Information|Debug"},
    {L"theme",             L"capture",         Kind::Combo, ID_THEME,      L"system|light|dark", L"Same as Windows|Light|Dark"},
    {L"check_updates",     L"capture",         Kind::Check, ID_CHECKUPDATES, nullptr},
    {L"start_with_windows", L"capture",        Kind::Check, ID_AUTOSTART,  nullptr},   // last: the registry is applied after the file (DoSave)
};

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
const Field* FindField(const std::wstring& key) {
    for (const Field& f : kFields) if (key == f.key) return &f;
    return nullptr;
}
const Field* FindFieldById(int id) {
    for (const Field& f : kFields) if (f.id == id) return &f;
    return nullptr;
}

std::wstring FloatText(double v) {
    wchar_t b[32];
    swprintf_s(b, L"%.6g", v);
    return b;
}

// Canonical form of a value, only used to decide "did this line change" (never written).
std::wstring Norm(const std::wstring& key, const std::wstring& raw) {
    const std::wstring v = Trim(raw);
    const Field* f = FindField(key);
    if (!f) return v;
    if (f->kind == Kind::Check) {
        const std::wstring l = Lower(v);
        if (l == L"1" || l == L"true" || l == L"yes" || l == L"on") return L"1";
        if (l == L"0" || l == L"false" || l == L"no" || l == L"off") return L"0";
        return l;
    }
    if (key == L"quality_vs_speed" && Lower(v) == L"auto") return L"auto";   // else numeric (below)
    if (key == L"rate_control") {
        const std::wstring l = Lower(v);
        return l == L"cqp" ? L"qp" : l;   // the parser takes cqp for qp
    }
    if (f->kind == Kind::Track || key == L"bitrate_mbps" || key == L"yt8k_bitrate_mbps" || key == L"max_fps" || key == L"max_width" || key == L"output_fov_scale" ||
        key == L"qp" || key == L"quality_vs_speed" || key == L"bit_depth" || key == L"sharpen" ||
        key == L"fill_fade_s" || key == L"indicator_x" || key == L"indicator_y" || key == L"indicator_z" ||
        key == L"indicator_size" || key == L"indicator_opacity") {
        wchar_t* end = nullptr;
        double d = wcstod(v.c_str(), &end);
        if (!v.empty() && end && *end == L'\0') {
            // the parser rounds max_width down to even (src/config.cpp): 3841 is shown as 3840 and must not count as a change
            if (key == L"max_width" && d > 0.0 && d < 1e9 && d == std::floor(d)) d = (double)((unsigned long long)d & ~1ull);
            return FloatText(d);
        }
        return Lower(v);
    }
    if (key == L"codec" || key == L"yt8k_codec") {
        const std::wstring l = Lower(v);
        if (l == L"h265" || l == L"h.265") return L"hevc";
        if (l == L"h.264" || l == L"avc") return L"h264";
        return l;
    }
    if (key == L"log_level") {
        const std::wstring l = Lower(v);
        return l == L"warning" ? L"warn" : (l == L"off" ? L"none" : l);
    }
    if (key == L"projection" || key == L"fill" || key == L"layout_3d" || key == L"post_when" || key == L"eye_crop" || key == L"theme")
        return Lower(v);
    if (key == L"vr_hotkey") {
        VrHotkey h;
        if (ParseVrHotkey(v, h)) return h.text;   // canonical ("thumbsticks", "off", "left_menu+right_a")
        return Lower(v);
    }
    if (key == L"hotkey") {
        Hotkey h;
        if (ParseHotkey(v, h)) return Lower(h.text);   // canonical list (device names keep their inner spaces)
        std::wstring r;
        for (wchar_t c : v) if (!iswspace(c)) r += (wchar_t)towlower(c);
        size_t p;
        while ((p = r.find(L"control")) != std::wstring::npos) r.replace(p, 7, L"ctrl");
        return r;
    }
    if (key == L"output_dir") {
        std::wstring r = v;
        while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
        return r;
    }
    return v;
}

std::string Utf8(const std::wstring& w) { return elog::Narrow(w); }
std::wstring FromUtf8(const std::string& s) { return elog::Widen(s); }

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    f.flush();
    return (bool)f;
}

bool IsHeader(const std::wstring& trimmed, std::wstring* name = nullptr) {
    if (trimmed.size() >= 2 && trimmed.front() == L'[' && trimmed.back() == L']') {
        if (name) *name = Lower(Trim(trimmed.substr(1, trimmed.size() - 2)));
        return true;
    }
    return false;
}

// Splits `key = value  ; comment` the way the parser reads it. Returns false for non-key lines.
struct ParsedLine {
    size_t keyStart = 0, keyEnd = 0;     // key text in the raw line
    size_t valStart = 0, valEnd = 0;     // trimmed value (still with quotes) in the raw line
    std::wstring key, value;             // lowercase key; value unquoted, comment stripped
    bool quoted = false;
};
bool ParseLine(const std::wstring& raw, ParsedLine& p) {
    const std::wstring t = Trim(raw);
    if (t.empty() || t[0] == L';' || t[0] == L'#' || t[0] == L'[') return false;
    const size_t eq = raw.find(L'=');
    if (eq == std::wstring::npos) return false;
    size_t ks = 0;
    while (ks < eq && iswspace(raw[ks])) ++ks;
    size_t ke = eq;
    while (ke > ks && iswspace(raw[ke - 1])) --ke;
    size_t vs = eq + 1;
    while (vs < raw.size() && iswspace(raw[vs])) ++vs;
    size_t ve = raw.size();
    for (const wchar_t* cm : {L" ;", L"\t;", L" #", L"\t#"}) {
        const size_t c = raw.find(cm, vs);
        if (c != std::wstring::npos && c < ve) ve = c;
    }
    while (ve > vs && iswspace(raw[ve - 1])) --ve;
    p.keyStart = ks; p.keyEnd = ke; p.valStart = vs; p.valEnd = ve;
    p.key = Lower(raw.substr(ks, ke - ks));
    p.value = raw.substr(vs, ve - vs);
    p.quoted = false;
    if (p.value.size() >= 2 && p.value.front() == L'"' && p.value.back() == L'"') {
        p.value = p.value.substr(1, p.value.size() - 2);
        p.quoted = true;
    }
    return true;
}

void InsertInSection(std::vector<std::wstring>& lines, const std::wstring& section, const std::wstring& text) {
    size_t header = (size_t)-1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring name;
        if (IsHeader(Trim(lines[i]), &name) && name == section) { header = i; break; }
    }
    if (header == (size_t)-1) {
        if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back(L"");
        lines.push_back(L"[" + section + L"]");
        lines.push_back(text);
        return;
    }
    size_t last = header;
    for (size_t i = header + 1; i < lines.size(); ++i) {
        if (IsHeader(Trim(lines[i]))) break;
        if (!Trim(lines[i]).empty()) last = i;
    }
    lines.insert(lines.begin() + (std::ptrdiff_t)last + 1, text);
}

std::wstring TempPath() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[64];
    swprintf_s(name, L"eyecap-settings-%lu.ini", GetCurrentProcessId());
    return std::wstring(tmp) + name;
}

// Folder of the running exe: where the Run entry should point.
std::wstring ExeFolder() {
    wchar_t p[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p, n < MAX_PATH * 2 ? n : 0);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : s.substr(0, k);
}

// The check box state: a Run value that starts THIS folder's tray and that Windows has not disabled (Task Manager). A value that
// points at another folder shows as unticked, so ticking it is the user action that repairs it.
bool AutostartChecked() {
    const install::AutostartInfo i = install::ReadAutostart();
    return (i.runPresent || i.oldPresent) && !i.disabledInTaskManager && install::AutostartPointsHere(i, ExeFolder());
}

Reporter g_reporter = nullptr;
void Report(const std::wstring& title, const std::wstring& text) {
    if (g_reporter) { g_reporter(title, text); return; }
    glass::MessageBox(DialogWindow(), text, title, MB_OK | MB_ICONWARNING);
}

} // namespace

void SetReporter(Reporter r) { g_reporter = r; }

KeyValues FromConfig(const Config& c) {
    KeyValues kv;
    auto B = [](bool b) { return std::wstring(b ? L"1" : L"0"); };
    std::wstring hk = c.hotkey.text;
    if (hk.empty()) hk = kDefaultHotkey;
    kv.emplace_back(L"hotkey", hk);
    kv.emplace_back(L"vr_hotkey", c.vrHotkey.text.empty() ? std::wstring(kDefaultVrHotkey) : c.vrHotkey.text);
    kv.emplace_back(L"output_dir", c.outputDir);
    kv.emplace_back(L"codec", FromUtf8(VideoCodecKey(c.codec)));
    kv.emplace_back(L"bitrate_mbps", std::to_wstring(c.bitrateMbps));
    kv.emplace_back(L"rate_control", FromUtf8(RateControlKey(c.rateControl)));
    kv.emplace_back(L"qp", std::to_wstring(c.qp));
    kv.emplace_back(L"quality_vs_speed", c.qualityVsSpeed < 0 ? std::wstring(L"auto") : std::to_wstring(c.qualityVsSpeed));
    kv.emplace_back(L"bit_depth", std::to_wstring(c.bitDepth));
    {
        wchar_t sh[32];
        swprintf_s(sh, L"%g", c.sharpen);
        kv.emplace_back(L"sharpen", sh);
    }
    kv.emplace_back(L"max_fps", std::to_wstring(c.maxFps));
    kv.emplace_back(L"constant_frame_rate", B(c.constantFrameRate));
    kv.emplace_back(L"beeps", B(c.beeps));
    kv.emplace_back(L"max_width", std::to_wstring(c.maxWidth));
    kv.emplace_back(L"require_tray", B(c.requireTray));
    kv.emplace_back(L"swap_eyes", B(c.swapEyes));
    kv.emplace_back(L"record_overlays", B(c.recordOverlays));
    kv.emplace_back(L"record_for_youtube", B(c.recordForYoutube));
    kv.emplace_back(L"tag_3d", B(c.tag3d));
    kv.emplace_back(L"symmetric_fov", B(c.head.symmetricFov));
    kv.emplace_back(L"head_smoothing", FloatText(c.head.smoothing));
    kv.emplace_back(L"roll_lock", B(c.head.rollLock));
    kv.emplace_back(L"output_fov_scale", FloatText(c.head.fovScale));
    kv.emplace_back(L"projection", FromUtf8(DomeProjectionName(c.dome.projection)));
    static const wchar_t* yl[] = {L"tb_half", L"sbs_half", L"sbs_full", L"tb_full"};
    kv.emplace_back(L"layout_3d", yl[(int)c.layout3d >= 0 && (int)c.layout3d < 4 ? (int)c.layout3d : 0]);
    kv.emplace_back(L"fill", c.dome.fill == DomeFill::LastSeen ? L"last_seen" : L"black");
    kv.emplace_back(L"fill_fade_s", FloatText(c.dome.fadeS));
    kv.emplace_back(L"dome_shared_view", B(c.dome.sharedView));
    kv.emplace_back(L"vr_indicator", B(c.indicator.enabled));
    kv.emplace_back(L"indicator_x", FloatText(c.indicator.x));
    kv.emplace_back(L"indicator_y", FloatText(c.indicator.y));
    kv.emplace_back(L"indicator_z", FloatText(c.indicator.z));
    kv.emplace_back(L"indicator_size", FloatText(c.indicator.size));
    kv.emplace_back(L"indicator_opacity", FloatText(c.indicator.opacity));
    kv.emplace_back(L"indicator_live", B(c.indicator.live));
    kv.emplace_back(L"audio_enabled", B(c.audioEnabled));
    kv.emplace_back(L"audio_device", c.audioDevice);
    kv.emplace_back(L"theme", c.uiTheme == 1 ? L"light" : c.uiTheme == 2 ? L"dark" : L"system");
    kv.emplace_back(L"check_updates", B(c.checkUpdates));
    kv.emplace_back(L"start_with_windows", B(c.startWithWindows));
    kv.emplace_back(L"steamvr_mode", elog::Widen(SteamVrModeKey(c.steamvr.mode)));
    kv.emplace_back(L"post_mkv", B(c.postMkv));
    kv.emplace_back(L"keep_mp4", B(c.keepMp4));
    kv.emplace_back(L"post_flat", B(c.postFlat));
    kv.emplace_back(L"post_yt8k", B(c.postYt8k));
    kv.emplace_back(L"yt8k_bitrate_mbps", std::to_wstring(c.yt8kBitrateMbps));
    kv.emplace_back(L"yt8k_codec", c.yt8kCodec == VideoCodec::Av1 ? L"av1" : L"hevc");
    static const wchar_t* pw[] = {L"now", L"session_end", L"game_exit", L"manual"};   // index = (int)PostWhen
    constexpr int kPw = (int)(sizeof(pw) / sizeof(pw[0]));
    kv.emplace_back(L"post_when", pw[(int)c.postWhen >= 0 && (int)c.postWhen < kPw ? (int)c.postWhen : (int)PostWhen::GameExit]);
    static const wchar_t* ec[] = {L"16:9", L"4:3", L"none", L"4:3pad"};   // index = (int)EyeCrop
    constexpr int kEc = (int)(sizeof(ec) / sizeof(ec[0]));
    kv.emplace_back(L"eye_crop", ec[(int)c.eyeCrop >= 0 && (int)c.eyeCrop < kEc ? (int)c.eyeCrop : 0]);
    static const wchar_t* lv[] = {L"none", L"error", L"warn", L"info", L"debug"};   // index = (int)LogLevel + 1
    kv.emplace_back(L"log_level", lv[(int)c.logLevel >= -1 && (int)c.logLevel < 4 ? (int)c.logLevel + 1 : 3]);
    return kv;
}

bool SaveKeyValuesToFile(const std::wstring& path, const KeyValues& kv, SaveResult& r) {
    r = SaveResult();
    // ---- cheap checks on the values themselves
    for (const auto& e : kv) {
        const std::wstring v = Trim(e.second);
        if (e.first == L"hotkey") {
            Hotkey h;
            if (!ParseHotkey(v, h))
                r.problems.push_back(L"hotkey: '" + v + L"' is not a valid hotkey (examples: Ctrl+Shift+F10, ScrollLock, MouseX1, "
                                     L"Pad:A, Pad:Back+Start, Joy:*:Button3, Joy:T300RS:Button5+Button6, none; "
                                     L"several separated by commas)");
        }
        if (v.find(L" ;") != std::wstring::npos || v.find(L" #") != std::wstring::npos ||
            v.find(L"\t;") != std::wstring::npos || v.find(L"\t#") != std::wstring::npos ||
            v.find(L'\r') != std::wstring::npos || v.find(L'\n') != std::wstring::npos)
            r.problems.push_back(e.first + L": the value must not contain ' ;', ' #' or a line break (config.ini reads those as a comment)");
    }
    if (!r.problems.empty()) return false;

    // ---- read the current file (may not exist yet)
    std::string bytes;
    const bool existed = ReadFileBytes(path, bytes);
    bool bom = false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bom = true;
        bytes.erase(0, 3);
    }
    const bool crlf = bytes.find("\r\n") != std::string::npos || !existed;
    const std::wstring text = FromUtf8(bytes);
    const bool endsWithNewline = text.empty() || text.back() == L'\n';
    std::vector<std::wstring> lines;
    {
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find(L'\n', pos);
            if (nl == std::wstring::npos) nl = text.size();
            std::wstring ln = text.substr(pos, nl - pos);
            if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
            lines.push_back(ln);
            pos = nl + 1;
        }
    }

    // ---- rewrite / append
    Config defCfg;
    ParseHotkey(kDefaultHotkey, defCfg.hotkey);
    const KeyValues defaults = FromConfig(defCfg);
    auto defaultOf = [&](const std::wstring& key) {
        for (const auto& e : defaults) if (e.first == key) return e.second;
        return std::wstring();
    };
    for (const auto& e : kv) {
        const Field* f = FindField(e.first);
        if (!f) continue;
        const std::wstring nv = Trim(e.second);
        // the parser lets the LAST occurrence win
        size_t at = (size_t)-1;
        ParsedLine pl, found;
        for (size_t i = 0; i < lines.size(); ++i)
            if (ParseLine(lines[i], pl) && pl.key == e.first) { at = i; found = pl; }
        if (at == (size_t)-1) {
            if (Norm(e.first, nv) == Norm(e.first, defaultOf(e.first))) continue;   // missing and default: leave the file alone
            InsertInSection(lines, f->section, e.first + L" = " + nv);
            r.changed.push_back(e.first + L" (added) " + nv);
            continue;
        }
        if (Norm(e.first, found.value) == Norm(e.first, nv)) continue;
        std::wstring& raw = lines[at];
        const std::wstring q = found.quoted && nv.find(L'"') == std::wstring::npos ? L"\"" : L"";
        const std::wstring old = found.value;
        raw = raw.substr(0, found.valStart) + q + nv + q + raw.substr(found.valEnd);
        r.changed.push_back(e.first + L" " + (old.empty() ? L"(empty)" : old) + L"->" + (nv.empty() ? L"(empty)" : nv));
    }
    if (r.changed.empty()) return true;   // nothing to write

    // ---- assemble the candidate
    const std::string eol = crlf ? "\r\n" : "\n";
    std::string out;
    if (bom) out += "\xEF\xBB\xBF";
    for (size_t i = 0; i < lines.size(); ++i) {
        out += Utf8(lines[i]);
        if (i + 1 < lines.size() || endsWithNewline || !existed || !r.changed.empty()) out += eol;
    }

    // ---- validate the candidate with the real parser; only problems that are NEW count
    const std::wstring tmp = TempPath();
    if (!WriteFileBytes(tmp, out)) {
        r.problems.push_back(L"cannot write the temporary file " + tmp);
        r.changed.clear();
        return false;
    }
    Config before, after;
    std::vector<std::wstring> oldProblems, newProblems;
    if (existed) ParseConfigFile(path, before, oldProblems);
    ParseConfigFile(tmp, after, newProblems);
    DeleteFileW(tmp.c_str());
    // compared without the "line N: " prefix (a key inserted above a bad line shifts its number; it is still the same
    // problem), as a multiset: each old problem excuses one new one
    auto withoutLineNo = [](const std::wstring& p) {
        if (p.compare(0, 5, L"line ") != 0) return p;
        size_t i = 5;
        while (i < p.size() && iswdigit(p[i])) ++i;
        if (i == 5 || p.compare(i, 2, L": ") != 0) return p;
        return p.substr(i + 2);
    };
    std::vector<std::wstring> known;
    for (const auto& o : oldProblems) known.push_back(withoutLineNo(o));
    for (const auto& p : newProblems) {
        const std::wstring s = withoutLineNo(p);
        bool found = false;
        for (auto it = known.begin(); it != known.end(); ++it)
            if (*it == s) { known.erase(it); found = true; break; }
        if (!found) r.problems.push_back(p);
    }
    if (!r.problems.empty()) { r.changed.clear(); return false; }

    // ---- write: temp file next to the target, then replace
    const std::wstring part = path + L".settings-tmp";
    if (!WriteFileBytes(part, out) || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(part.c_str());
        // fall back to an in-place write (e.g. the folder does not allow new files but the file is writable)
        if (!WriteFileBytes(path, out)) {
            r.problems.push_back(L"cannot write " + path + L" (is it read-only or open in another program?)");
            r.changed.clear();
            return false;
        }
    }
    return true;
}

bool SaveSettingsToFile(const std::wstring& path, const Config& c, SaveResult& r) {
    // like the window with its "SteamVR games" drop-down untouched: steamvr_mode is left as the file has it (see DoSave)
    KeyValues kv = FromConfig(c);
    for (auto it = kv.begin(); it != kv.end(); ++it)
        if (it->first == L"steamvr_mode") { kv.erase(it); break; }
    return SaveKeyValuesToFile(path, kv, r);
}

bool HotkeyText(unsigned vk, bool ctrl, bool shift, bool alt, std::wstring& out) {
    return KeyBindingText(vk, ctrl, shift, alt, out);   // one naming table: src/hotkey.cpp
}

// ================================================================================================ the window
namespace {

HWND                g_dlg = nullptr;
HWND                g_tip = nullptr;            // the tooltip window (owned by the dialog)
HFONT               g_font = nullptr;
HFONT               g_odFont = nullptr;     // g_font with grey (not ClearType) smoothing: the owner-drawn previews (glass two-pass alpha)
HFONT               g_odFontLg = nullptr;   // g_odFont at 96 DPI: text drawn through a scaling world transform (the previews)
HFONT               g_logoFont = nullptr;      // the bold wordmark font of the logo (ID_LOGO): the message font at 20 px, FW_BOLD
HWND                g_hover = nullptr;          // the control the help panel last followed (mouse or keyboard focus)
const wchar_t*      g_shown = nullptr;          // the text now in the help panel (pointer compare: no flicker)
std::wstring        g_path;
bool                g_done = false;
int                 g_capturing = 0;           // "Press a key": 0 = off, 1 = replace the field (ID_HOTKEY_PICK), 2 = add (ID_HOTKEY_ADD)
int                 g_smoothPos = -1;       // trackbar position / text as loaded: an untouched slider keeps the exact file value
std::wstring        g_smoothText;
std::vector<std::pair<int, std::wstring>> g_customValue;   // combo id -> the file value its "Custom (...)" item stands for
std::function<void(const std::vector<std::wstring>&)> g_onSaved;
std::function<void()> g_openSteamVr;      // SetOpenSteamVrHook
QueueHooks          g_hooks;                // conversion worker callbacks (SetQueueHooks)
ToolHooks           g_tools;                // the Advanced "Tools:" row (SetToolHooks)
UpdateHooks         g_update;               // the Advanced "Updates:" row + the "update available" line (SetUpdateHooks)
std::vector<unsigned long long> g_qIds;     // job ids of the waiting-list items, same order as the list box
std::vector<std::wstring> g_qItems;         // their texts (the list is only rebuilt when this changes: no flicker)
bool                g_qRunNow = false;      // the Pause button reads "Run now" (the running job waits for the hold)
const UINT          WM_QUEUE_POKE = WM_APP + 20;   // NotifyQueueChanged -> refresh the queue view
const UINT          WM_UPDATE_POKE = WM_APP + 21;  // NotifyUpdateChanged -> refresh the "update available" line
const UINT_PTR      kQueueTimer = 5;        // 500 ms: refresh the queue view (progress / ETA) even without a poke
std::vector<HWND>   g_dim;                  // static texts drawn in the grey hint colour
std::vector<HWND>   g_footer;               // footer controls: kept at the bottom of the window while the content scrolls
int                 g_contentH = 0;         // client height of the content above the footer
int                 g_footerH = 0;
int                 g_viewH = 0;            // visible content height when the window is shorter than the content (scrolling)
int                 g_scrollY = 0, g_scrollMax = 0;   // scroll offset / its maximum (0 = nothing to scroll)
std::vector<HWND>   g_made;                 // every control made by Builder::Make, in creation order (the Move pass walks it)
int                 g_helpMinH = 0;         // height of the help edit that shows the longest help text without cut (MeasureHelp)
std::wstring        g_loadedSvrMode;           // steamvr_mode as loaded into the window (DoSave writes it only when changed)
KeyValues           g_savedKv;              // the window's values as last loaded / saved (ComparableValues): Exit asks when they differ
const UINT_PTR      kSavedTimer = 7;        // one-shot: the Save button reads "Saved" for a moment after a save
int                 g_helpPad = 0;          // help edit client height minus its default formatting rectangle height
int                 g_naturalH = 0;         // content height with the help edit at g_helpMinH (independent of the window size)
size_t              g_contentEnd = 0;       // g_made index of the first footer control (set by the Create pass)
bool                g_footerRaised = false; // the footer is above the content in the z-order (only while scrolling)
// Tabs: every control made by Builder::Make belongs to one tab (g_madeTab, same index as g_made) or to none (-1: header,
// tab strip, Description, footer). Only the controls of g_tab are shown. A control the code hides on its own (the update
// line, the frame-rate warning, Register...) is in g_wantHidden, so a tab switch does not show it again (SetShown).
const int           kTabCount = 4;
const wchar_t* const kTabNames[kTabCount] = {L"Recording", L"Video", L"Export", L"App"};
const wchar_t* const kTabIcons[kTabCount] = {L"\xE7C8", L"\xE714", L"\xE898", L"\xE713"};   // Segoe Fluent Icons / MDL2: Record, Video, Upload, Settings
int                 g_tab = 0;              // the tab shown (kept while the tray runs: the window reopens on it)
int                 g_tabHover = -1;        // the tab under the mouse (-1 = none)
std::vector<int>    g_madeTab;              // tab of g_made[i]
std::vector<HWND>   g_wantHidden;           // controls hidden by their own logic
HFONT               g_tabFont = nullptr;    // semibold message font, grey smoothing (owner-drawn tab strip)
HFONT               g_iconFont = nullptr;   // Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10); nullptr = no icons
const int           kLayoutW = 1140;        // the narrowest layout: client width without the scroll bar
const int           kMinClientH = 400;      // minimum client height (the content scrolls below its natural height)
const int           kFooterH = 64;
const int           kFooterBgId = 3001;     // opaque owner-drawn strip behind the footer buttons
// DPI: the window is made per-monitor DPI-aware (ShowDialog), so Windows does not stretch it and its text is sharp. The
// layout works in 96-DPI units ("layout units": every constant in BuildControls); Builder::Make turns them into device pixels
// at the DPI of the screen the window is on (WM_DPICHANGED when it moves to another one).
double              g_s = 1.0;              // device pixels per layout unit (the window's DPI / 96)
UINT Dpi() { return (UINT)std::lround(g_s * 96); }
int Px(int v) { return (int)std::lround(v * g_s); }
int Lg(int px) { return (int)std::ceil(px / g_s - 1e-6); }   // device pixels -> layout units, rounded up (text never cut)
// The client is never narrower than kLayoutW, even with the vertical scroll bar shown (layout units).
int MinClientW() { return kLayoutW + Lg(GetSystemMetricsForDpi(SM_CXVSCROLL, Dpi())); }
// The window rectangle for a client rectangle: AdjustWindowRectEx without the title bar (DlgProc WM_NCCALCSIZE removes it).
void AdjustFrame(RECT& r, DWORD style) {
    const LONG top = r.top;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_APPWINDOW, Dpi());
    r.top = top;
}
void Relayout();
void TryClose();
void ApplyGlassTheme();
void MakeFonts(UINT dpi);
void MeasureHelp();


// ---- help texts: hover tooltip + the "Description" panel. Written for people, not for the log: a short first line that says what
// the setting does, a blank line, then one point per line ("• " bullets for choices). The panel is as tall as the longest
// text (MeasureHelp) and the window must still fit 1080p (settings_test), so keep every text at about 11 lines or less at the
// narrowest width (about 160 characters per line).
#define HELP_BR L"\r\n\r\n"
#define HELP_LI L"\r\n• "
const wchar_t* const kOverview =
    L"Point at a setting to see what it does here." HELP_BR
    L"How it works:"
    HELP_LI L"Press the hotkey in a VR game. VR EyeCapture copies the two eye images that the game sends to your headset and saves them as an MP4 file."
    HELP_LI L"OpenXR games are recorded by an OpenXR layer (eye_capture.dll). SteamVR games need a small one-time setup: footer button SteamVR games..."
    HELP_LI L"After the recording, this tray app turns the MP4 into a 3D MKV file that you can upload to YouTube (see Export)."
    HELP_LI L"Nothing is injected into the game. Recording only works while this tray app is running." HELP_BR
    L"Saved changes apply to the next recording.";

struct Help { int id; const wchar_t* text; };
const Help kHelp[] = {
    {ID_HOTKEY, L"The key or button that starts and stops a recording. Press it once to start, again to stop." HELP_BR
                L"• Pick a key the game does not use. ScrollLock works well. List several, separated by commas: any of them works."
                HELP_LI L"Keyboard keys and mouse buttons (MouseMiddle, MouseX1, MouseX2) are read when the game starts: restart the game after a change."
                HELP_LI L"Xbox controller: Pad:A, Pad:Back+Start (hold both), Pad2:LB (second controller only)."
                HELP_LI L"Wheel or joystick: Joy:*:Button3 (any device), Joy:T300RS:Button5+Button6 (a device whose name contains T300RS); POVUp / POVDown / POVLeft / POVRight for the hat switch."
                HELP_LI L"Controller and wheel buttons work at once, without a game restart." HELP_BR
                L"To see the button names of a wheel, run EyeCaptureTray.exe --inputs in a command prompt."},
    {ID_VRHOTKEY, L"Start and stop with the VR controllers: hold these buttons together for 1 second (OpenXR and SteamVR games)." HELP_BR
                  L"• Both stick clicks (default): press both thumbsticks down and hold them. Games rarely use that."
                  HELP_LI L"The controllers vibrate briefly when a recording starts and longer when it stops; the dot in the headset shows the state."
                  HELP_LI L"Works in any OpenXR or SteamVR game, also when the game has no keyboard support. The game still sees the buttons too."
                  HELP_LI L"Custom combinations in config.ini: vr_hotkey = left_menu+right_a (buttons a, b, x, y, menu, thumbstick, trigger, grip; "
                  L"left_ / right_; up to 4). The hold time is vr_hotkey_hold_s (default 1.0 s)."
                  HELP_LI L"Off: the layer does not touch the game's controller input at all." HELP_BR
                  L"Read when the game starts: restart the game after a change. SteamVR games: use the hotkey or the tray."},
    {ID_HOTKEY_PICK, L"Sets the hotkey by pressing it." HELP_BR
                     L"Click, then press a key, a mouse side button, a controller button (hold several for a combination) or a wheel button. "
                     L"It replaces what is in the field. Esc cancels."},
    {ID_HOTKEY_ADD, L"Adds one more hotkey by pressing it." HELP_BR
                    L"Click, then press a key, a mouse side button, a controller button (hold several for a combination) or a wheel button. "
                    L"It is added to the list, so the hotkeys you already have keep working. Esc cancels."},
    {ID_CODEC, L"The video format of the recording." HELP_BR
               L"• HEVC / H.265 (recommended): the best quality per megabyte. The format YouTube expects for large 3D video."
               HELP_LI L"H.264: older, plays in more programs, but files are bigger or softer. Limited to 4096 px wide."
               HELP_LI L"AV1: needs an NVIDIA RTX 40 / 50 card. About HEVC quality at two thirds of the bitrate (default 80 Mbit/s)." HELP_BR
               L"For 8K at VR frame rates the encoder uses its faster setting, so no frames are dropped. Without an AV1 encoder the recording is made in HEVC."},
    {ID_BITRATE, L"How much data per second the video gets. More = a sharper picture (most visible in fast motion) and bigger files." HELP_BR
                 L"• Default: 120 Mbit/s for HEVC, 80 Mbit/s for H.264 and AV1."
                 HELP_LI L"Suggested: 250 Mbit/s at 60 fps or more; the default at 30 fps."
                 HELP_LI L"One hour of recording: 120 Mbit/s = about 54 GB, 150 = 68 GB, 200 = 90 GB, 250 = 113 GB, 300 = 135 GB, "
                 L"400 = 180 GB, 500 = 225 GB."
                 HELP_LI L"350 to 500 Mbit/s: for VR180 / VR360, where you see more detail. Needs a fast GPU (RTX 40 / 50) and a fast disk." HELP_BR
                 L"A value from config.ini that is not in the list shows as Custom and is kept."},
    {ID_BITRATEHINT, L"The size of one hour of recording at this bitrate (video only; sound adds about 0.1 GB)."},
    {ID_YT8KBITRATE, L"How much data per second the 8K 3D MKV gets (only when YouTube output makes the 8K file)." HELP_BR
                     L"• 150 Mbit/s (default): keeps YouTube's 8K quality well fed."
                     HELP_LI L"200 Mbit/s: a little more detail in fast motion, bigger files. 100 Mbit/s: smaller files."
                     HELP_LI L"With the AV1 8K codec the same value looks better: about 100 Mbit/s AV1 looks like 150 Mbit/s HEVC." HELP_BR
                     L"Without an NVIDIA graphics card the slower x265 encoder is used at a fixed quality, and this setting does nothing. "
                     L"Greyed with 'Record directly in the YouTube layout': then the recording itself is the file to upload."},
    {ID_YT8KCODEC, L"The video format of the 8K 3D MKV (only when YouTube output makes the 8K file)." HELP_BR
                   L"• HEVC (default): works on every NVIDIA card with NVENC."
                   HELP_LI L"AV1: needs an NVIDIA RTX 40 / 50 card. A sharper picture at the same 8K bitrate. YouTube accepts it." HELP_BR
                   L"An AV1 file gets _AV1 in its name (<name>_YT8K_AV1.mkv), so an HEVC and an AV1 copy of one recording can sit side by side. "
                   L"If AV1 is not available, the file is made in HEVC with the plain name. Greyed with 'Record directly in the YouTube layout'."},
    {ID_OUTFREE, L"Free space on the drive of the output folder." HELP_BR
                 L"• Recordings and all converted files go there. Nothing large goes to drive C: unless the folder is on C:."
                 HELP_LI L"Below 10 GB free, recordings and conversions do not start."
                 HELP_LI L"Below 2 GB, a running recording stops and is saved."},
    {ID_FPSWARN, L"The game runs at a frame rate that does not divide evenly into 60 fps (72, 80, 90, 144 fps ...)." HELP_BR
                 L"YouTube plays at most 60 fps, and the 8K file is 60 fps. Some frames are then dropped unevenly, and motion stutters a little." HELP_BR
                 L"Fix: set the headset to 120 Hz (or 60 Hz) in Virtual Desktop or the headset settings."},
    {ID_MAXWIDTH, L"The width of the recorded video: both eyes side by side." HELP_BR
                  L"• Native: the full size from the headset. Very large on a Quest 3 (up to the codec limit)."
                  HELP_LI L"3840 px (4K): a good balance. 2560 px (2K) or 1920 px (1080p): for weaker graphics cards."
                  HELP_LI L"Smaller = less GPU work while you play and smaller files. The height follows automatically." HELP_BR
                  L"A value from config.ini that is not in the list shows as Custom and is kept."},
    {ID_MAXFPS, L"The highest frame rate of the recording." HELP_BR
                L"• 0: every frame the headset shows (72, 90, 120 fps ...)."
                HELP_LI L"60 or 30: smaller files and less GPU work. YouTube plays at most 60 fps."},
    {ID_RATECONTROL, L"How the encoder spends the bitrate." HELP_BR
                     L"• VBR (default): around the Bitrate setting. Busy scenes get more, calm scenes less."
                     HELP_LI L"CBR: always the Bitrate setting. Predictable file size, but calm scenes waste space."
                     HELP_LI L"Constant QP: every frame at the same quality (the QP field). Bitrate is not used; fast, detailed scenes make much bigger files." HELP_BR
                     L"With AV1, the QP is passed on as a quality value (the NVIDIA AV1 encoder has no real QP)."},
    {ID_QP, L"The quality of every frame with Constant QP (greyed for VBR and CBR)." HELP_BR
            L"• 0 = best quality and huge files, 51 = worst. Default 24."
            HELP_LI L"18 to 24 looks very good. Each step of 6 about halves or doubles the file size."},
    {ID_PRESET, L"How hard the graphics card works on each frame (quality_vs_speed in config.ini)." HELP_BR
                L"• Auto (default): the encoder's own choice. With AV1 at 8K and 72 / 90 fps it picks a fast setting, so no frames are dropped."
                HELP_LI L"Fastest: the least GPU time."
                HELP_LI L"Best quality: a sharper picture at the same bitrate, but slower. At high resolutions and frame rates it can drop frames." HELP_BR
                L"A value from config.ini that is not in the list shows as Custom and is kept."},
    {ID_BITDEPTH, L"Colour steps per channel in the video (bit_depth in config.ini)." HELP_BR
                  L"• 8-bit (default): plays everywhere."
                  HELP_LI L"10-bit: smooth skies and dark scenes without banding (HEVC Main10 / AV1 10-bit). Needs codec HEVC or AV1; "
                  L"H.264 records 8-bit. YouTube, VLC and Quest players play it; some older players and editors do not." HELP_BR
                  L"The file size does not change much."},
    {ID_SHARPEN, L"Sharpens the picture while it is recorded (sharpen in config.ini, 0 .. 1)." HELP_BR
                 L"• Off (default): no extra work."
                 HELP_LI L"Light / Medium / Strong: more fine detail. Most useful for VR180 / VR360, where the dome makes the picture softer." HELP_BR
                 L"Contrast-adaptive: flat areas get more, hard edges less, so there are no bright halos. Each eye is sharpened on "
                 L"its own. Works for flat recordings too. Sharper video needs a bit more bitrate."},
    {ID_CFR, L"Writes exactly one frame per frame period, all the same length." HELP_BR
             L"• Ticked: video editors such as DaVinci Resolve handle the file better. A doubled frame is dropped; a missing one repeats the last (up to 5 seconds)."
             HELP_LI L"Cleared (default): the real frame times are kept. Plays perfectly in players and on YouTube." HELP_BR
             L"The sound is not changed."},
    {ID_BEEPS, L"Beeps in the headset when a recording starts (one beep), stops (two beeps) or cannot start (one long low beep)." HELP_BR
               L"The beeps play on the Windows default playback device. That is usually the device being recorded, so the beeps are "
               L"in the video too. Clear this to keep them out."},
    {ID_OUTDIR, L"The folder where recordings are saved. The converted files go next to them." HELP_BR
                L"• Empty: the 'captures' folder next to the app."
                HELP_LI L"Pick a drive with lots of free space: about 54 GB per hour at the default bitrate."},
    {ID_BROWSE, L"Picks the output folder, on any drive. The free space of that drive is shown below the field."},
    {ID_AUDIO, L"Records the sound of your PC (the Windows playback device) into the video."},
    {ID_OVERLAYS, L"Records the menus and panels that float in front of you in the headset." HELP_BR
                  L"• Ticked: game menus and mod panels (such as the DLAA injector's VR panel) are in the video, placed as you saw them."
                  HELP_LI L"Cleared: only the game's 3D view is recorded." HELP_BR
                  L"The Virtual Desktop and Quest menus (and the guardian) come from the headset itself and are never recorded. Neither is the red recording dot."},
    {ID_AUDIODEV, L"Which playback device to record." HELP_BR
                  L"• Empty: the Windows default playback device."
                  HELP_LI L"Or part of a device name, for example 'Virtual Desktop Audio'."},
    {ID_PROJECTION, L"The shape of the video." HELP_BR
                    L"• Flat stereo (recommended): a normal side-by-side 3D video of exactly what you saw. Plays everywhere."
                    HELP_LI L"VR180 / VR360 (EXPERIMENTAL): a dome you can look around in with a VR player (YouTube VR, DeoVR, Virtual Desktop)." HELP_BR
                    L"The game only draws where you look (about 100 degrees). The dome shows live video there, older video where you looked "
                    L"before, and black where you never looked. Symmetric field of view and head smoothing are off in dome mode."},
    {ID_FILL, L"VR180 / VR360 only: what the dome shows where you are not looking now." HELP_BR
              L"• Hold last seen view: the last picture of that area stays until it fades."
              HELP_LI L"Black: only the live view; the rest is black."},
    {ID_FILLFADE, L"VR180 / VR360 only: how many seconds an older area stays before it fades to black (the fade takes 2 seconds)." HELP_BR
                  L"0 = never fade."},
    {ID_SHAREDVIEW, L"VR180 / VR360 only: each eye shows only the part of the view that the other eye also sees." HELP_BR
                    L"• Ticked (recommended): no one-eye strips at the sides. Easy on the eyes."
                    HELP_LI L"Cleared: each eye shows its full view. The video is wider (on a Quest 3 about 14 degrees more on each "
                    L"outer side), but only one eye sees those outer strips, which can strain the eyes."},
    {ID_SYMFOV, L"Makes the 3D comfortable to watch (flat stereo only)." HELP_BR
                L"The headset draws each eye with a lopsided view (wider on the outside). This turns both eyes into one even view, so a 3D "
                L"player can show them without eye strain. Costs a little GPU time. Clear it only to record the raw eye images."},
    {ID_SWAP, L"Swaps the left and right eye in the video. Use it if the 3D looks inside out."},
    {ID_ROLL, L"Keeps the horizon level in the video when you tilt your head (flat stereo with head smoothing above 0)."},
    {ID_SMOOTH, L"Takes the shake out of your head movement in the video (flat stereo only)." HELP_BR
                L"• 0 = off: the video follows your head exactly."
                HELP_LI L"Higher: small head movements are smoothed away. Big turns still follow your head."
                HELP_LI L"Costs a little GPU time. Black edges are prevented automatically (edge_guard in config.ini)."},
    {ID_FOVSCALE, L"How much of the game's view the video shows, with head smoothing on (0.5 to 1)." HELP_BR
                  L"Smoothing needs a margin at the edges to move the picture. 1 = no margin. 0.9 (default) = about 3 degrees on each side. "
                  L"Smaller values give more margin and show less."},
    {ID_INDICATOR, L"Shows a small red dot in the headset while a recording runs. The dot is never in the video." HELP_BR
                   L"Direct3D 11 games only (ETS2 / ATS). Read when the game starts."},
    {ID_INDLIVE, L"Turns the dot violet while OBS shows the live picture of the game (the VR EyeCapture source in OBS)." HELP_BR
                 L"\u2022 Ticked (default): you can see in the headset that OBS is watching, even when you are not recording."
                 HELP_LI L"Cleared: the dot only shows while recording." HELP_BR
                 L"The dot is never in the video or the stream. Read when the game starts."},
    {ID_INDX, L"Left / right position of the dot, in metres. 0 = centre, positive = right, negative = left. Default 0.22."},
    {ID_INDY, L"Up / down position of the dot, in metres. 0 = eye level, positive = up, negative = down. Default -0.16."},
    {ID_INDZ, L"Distance of the dot in front of you, in metres. Always negative: -0.8 = 80 cm ahead (default). Farther = smaller."},
    {ID_INDSIZE, L"Size of the dot, in metres." HELP_BR
                 L"• At -0.8 m: 0.02 is about 1.4 degrees, 0.005 is a small dot, 0.002 is a few pixels."
                 HELP_LI L"Below about 0.001 the dot is smaller than one headset pixel and disappears. The smallest value is 0.0005."},
    {ID_INDOPACITY, L"How see-through the dot is. 1 = solid, 0.3 = faint."},
    {ID_SVRMODE, L"How SteamVR games are recorded." HELP_BR
                 L"• Per game (best quality): only the games ticked in the SteamVR games window, from the game's own eye images. "
                 L"Head smoothing, symmetric view and VR180 / VR360 work."
                 HELP_LI L"Quick mode: no setup. Records SteamVR's own mirror picture: lower resolution, SteamVR menus visible, flat only, no dot. "
                 L"Ticked games still record per game."
                 HELP_LI L"Off: SteamVR games are not recorded." HELP_BR
                 L"The same setting as the drop-down at the top of the SteamVR games window."},
    {ID_INDPREVIEW, L"Where the dot is in your view. The circle is your field of view; the centre is straight ahead." HELP_BR
                    L"• Click or drag: put the dot there (X and Y follow)."
                    HELP_LI L"Mouse wheel: move it nearer or farther (Z)." HELP_BR
                    L"Click Save. The game uses the new place the next time it starts."},
    {ID_LAYOUTPREVIEW, L"A drawing of the files the tray makes, with their sizes in pixels." HELP_BR
                       L"• Left: the recording (example from a Quest 3: 8192x4630, both eyes side by side). Eye crop removes the hatched part."
                       HELP_LI L"Right: the 3D copy for YouTube, with the left (L) and right (R) eye placed as set in Output layout."
                       HELP_LI L"With 'Record directly in the YouTube layout': left is the headset view, right is the recording itself." HELP_BR
                       L"It follows Resolution, Codec, Output layout, Eye crop, YouTube output and Projection. VR180 / VR360 are copied unchanged."},
    {ID_YTMODE, L"Which files the tray makes after each recording, for YouTube." HELP_BR
                L"• 3D MKV: the recording with the 3D tags YouTube reads, in the Output layout. YouTube plays it in 4K quality."
                HELP_LI L"8K 3D MKV (sharper, slower): <name>_YT8K.mkv, 60 fps, made on the GPU in about twice the clip length. "
                L"YouTube keeps its 8K quality and a much higher bitrate, so 3D looks much sharper in the headset. Flat 3D only."
                HELP_LI L"With 'Record directly in the YouTube layout' the recording already is that file: its 3D MKV is a quick copy." HELP_BR
                L"Upload the .mkv file named in the notification. Files are never overwritten: a new one gets \" (2)\" in its name. "
                L"Crop and AV1 add tags to the name (_4x3, _full, _AV1)."},
    {ID_TAG3DMP4, L"Writes the 3D tag (and the VR180 / VR360 tag) into the recorded MP4, so players and YouTube show it in 3D." HELP_BR
                  L"Clear it to get a plain MP4. Video editors (for example DaVinci Resolve) open a plain MP4 more easily." HELP_BR
                  L"The 3D MKV still gets its own 3D tag. To add the tag to a plain MP4 later, use Tag as 3D... on the App tab."},
    {ID_RECORDYT, L"Records straight into the YouTube layout, so nothing is re-encoded afterwards." HELP_BR
                  L"• Each frame is written in the Output layout, with Eye crop applied, 4320 px on the short side (Quest 3 at 16:9: 7680x4320)."
                  HELP_LI L"The tray only copies it into the 3D MKV (seconds). Upload that MKV."
                  HELP_LI L"Output layout and Eye crop cannot be changed later. No separate 8K file is made; a 2D video still can be."
                  HELP_LI L"Frames wider than 4096 px need HEVC or AV1. Use a 60 fps frame rate limit (YouTube plays at most 60 fps)." HELP_BR
                  L"Flat recordings only; VR180 and VR360 ignore it."},
    {ID_LAYOUT3D, L"How the two eyes are placed in the 3D files (the 3D MKV and the 8K 3D MKV)." HELP_BR
                  L"• Top/bottom, half height (YouTube): left eye on top, each eye squeezed to half height. Re-encoded."
                  HELP_LI L"Side by side, half width: each eye squeezed to half width. Re-encoded."
                  HELP_LI L"Side by side, full: the recording as it is. A fast copy, but YouTube shows it stretched: for headset players and 3D TVs."
                  HELP_LI L"Top/bottom, full: each eye at its own shape, left eye on top. Re-encoded." HELP_BR
                  L"The recording itself stays side by side, unless 'Record directly in the YouTube layout' is ticked. Greyed for VR180 / VR360."},
    {ID_EYECROP, L"Cuts the top and bottom off each eye before the 3D files are made. Headset eyes are tall, and a tall video crashes the "
                 L"YouTube player on Quest and stays at 2160p. Nothing is stretched." HELP_BR
                 L"• 16:9 (recommended): wide view. The 8K file is 7680x4320: plays on Quest, gets YouTube's 8K quality."
                 HELP_LI L"4:3: taller view, 5760x4320. YouTube probably treats it as 4K, so it looks softer."
                 HELP_LI L"4:3, padded to 16:9: the taller view with black bars left and right. 7680x4320, gets 8K quality."
                 HELP_LI L"None: the full, tall eye. The Quest YouTube player may crash, and YouTube stays at 2160p." HELP_BR
                 L"An eye already wider than the shape is not cut. Cropped files get a tag (_4x3, _4x3pad, _full), so two versions can sit side by side."},
    {ID_KEEPMP4, L"Keeps the original MP4 next to the MKV files." HELP_BR
                 L"Cleared: the MP4 is deleted once the 3D MKV is made and checked. It is always kept when no 3D MKV is made, and while "
                 L"another conversion of it still waits."},
    {ID_POSTWHEN, L"When the tray converts a finished recording. Converting uses the CPU and GPU and can lower the game's frame rate." HELP_BR
                  L"• Immediately: as soon as the recording is saved."
                  HELP_LI L"When the VR session ends: once no game is in VR. If VR starts again, the conversion pauses and continues later."
                  HELP_LI L"When the game exits (default): once the game that made the recording has closed."
                  HELP_LI L"When I say: never on its own. Recordings wait in the Conversions list; click Start or Start all." HELP_BR
                  L"'Convert a recording...' always starts at once; 'Add to queue...' always waits for Start."},
    {ID_CONVERTPICK, L"Converts MP4 files you pick, right after the one that is running now." HELP_BR
                     L"It uses the Export settings shown in this window now, even if they are not saved, and ignores the Convert setting. "
                     L"Pick several files with Ctrl or Shift; they convert one after another."},
    {ID_ADDQUEUE, L"Adds MP4 files you pick to the Conversions list. They wait there until you click Start or Start all." HELP_BR
                  L"They use the Export settings shown in this window now (saved or not). To make several versions of one recording "
                  L"(for example 16:9 and 4:3, or HEVC and AV1), change the settings and add the same file again. Each version gets its own name."},
    {ID_QSTATUS, L"The conversion running now: the file, what it makes, the step (MKV, 2D or 8K), progress and time left." HELP_BR
                 L"If it waits for the Convert setting (for example while the game still runs), it says why."},
    {ID_QPROGRESS, L"Progress of the running step. Yellow while paused. (Needs ffprobe, which comes with ffmpeg.)"},
    {ID_QPAUSE, L"Pauses the running conversion without losing progress. Resume continues it." HELP_BR
                L"If the conversion waits for the Convert setting, the button reads Run now and starts it at once."},
    {ID_QCANCEL, L"Stops the running conversion and deletes its unfinished file. The MP4 is kept, so you can convert it later. "
                 L"Waiting conversions carry on."},
    {ID_QLIST, L"Conversions waiting, in the order they run: file, format and state." HELP_BR
               L"• waiting for you: added with Add to queue (or restored after a restart). Waits for Start."
               HELP_LI L"waiting: converts when ...: starts when the Convert setting allows."
               HELP_LI L"(current settings): an automatic recording. It uses the saved settings at the moment it starts." HELP_BR
               L"Select several with Ctrl or Shift. The list is kept when the tray app closes."},
    {ID_QSTART, L"Starts the selected conversions, one after another, whatever the Convert setting says."},
    {ID_QSTARTALL, L"Starts every waiting conversion, one after another in list order, whatever the Convert setting says."},
    {ID_QREMOVE, L"Removes the selected conversions from the list. The MP4 stays; convert it later with 'Convert a recording...'."},
    {ID_POSTFLAT, L"Also makes a normal 2D video from the left eye (<name>_FLAT.mp4), for people without a headset. It takes a while."},
    {ID_LOGLEVEL, L"How much the app writes to its log files (logs\\eye_capture.log and logs\\tray.log)." HELP_BR
                  L"• Information: normal."
                  HELP_LI L"Debug: more detail, for bug reports."
                  HELP_LI L"Off: nothing."},
    {ID_AUTOSTART, L"Starts this tray app when you sign in to Windows, so it is ready before you start a VR game." HELP_BR
                   L"• Off by default. It writes one 'Run' entry for your Windows account; turning it off removes it."
                   HELP_LI L"If the entry goes missing (a cleaner tool, a moved folder), the app puts it back at its next start."
                   HELP_LI L"The switch in Task Manager > Startup apps belongs to Windows: this app never changes it." HELP_BR
                   L"Clear this before you delete the app's folder."},
    {ID_OPENLOG, L"Opens the log file (logs\\eye_capture.log). Useful when something does not work, and for bug reports."},
    {ID_TAG3D, L"Marks a video you already have as 3D, so YouTube and VR players show it in 3D." HELP_BR
               L"Pick how the eyes are placed in it (side by side or top / bottom), then the video. A tagged copy (.mkv) is made next to it; "
               L"the video itself is not re-encoded."},
    {ID_REGLAYER, L"Turns recording of OpenXR games on or off for this PC." HELP_BR
                  L"The installer does this for you. Use it only to repair or to switch it off. Windows asks for administrator rights."},
    {ID_THEME, L"The look of this window, the SteamVR games window and the tray menu." HELP_BR
               L"\u2022 Same as Windows (default): follows the Windows light / dark setting."
               HELP_LI L"Light or Dark: always that, whatever Windows uses." HELP_BR
               L"Changes when you click Save."},
    {ID_OPENINI, L"Opens config.ini in a text editor. It lists every setting, with comments."},
    {ID_DEFAULTS, L"Puts every field back to its default. Nothing is saved until you click Save."},
    {ID_CHECKUPDATES, L"Check GitHub for a newer version (check_updates)." HELP_BR
                      L"On (default): a few seconds after the app starts, and then once a day, it asks GitHub whether a newer "
                      L"release exists -- one quick request, nothing is downloaded. When there is one you get a tray balloon and a line here." HELP_BR
                      L"Downloading and installing only ever happen when you click to update. Off: it never checks on its own."},
    {ID_UPDATENOW, L"Checks GitHub for a newer version right now. Works even when the automatic check is off." HELP_BR
                   L"Nothing is downloaded by this -- it only looks."},
    {ID_UPDATELINK, L"A newer version is available. Click \"Update now\" to download the installer from GitHub (its checksum is "
                    L"verified) and run it; you will see the normal Windows (UAC) prompt. The same action is in the tray menu."},
    {ID_OPENSTEAMVR, L"Opens the SteamVR games window: tick the SteamVR games to record, or choose Quick mode. OpenXR games need nothing there."},
    {ID_SAVE, L"Saves the changes to config.ini. The window stays open. The next recording uses them."},
    {ID_MINIMIZE, L"Minimizes the window to the taskbar."},
    {ID_TABS, L"The pages of this window: Recording, Video, Export and App." HELP_BR
              L"Click a tab, or press Ctrl+Tab / Ctrl+Shift+Tab. Save saves the settings of all pages."},
    {ID_CANCEL, L"Closes the window. If you have unsaved changes, it asks first. The tray app keeps running."},
    {ID_ABOUTLINK, L"The project page on GitHub: downloads, the manual and bug reports." HELP_BR
                   L"• License: PolyForm Noncommercial 1.0.0. Copyright (c) 2026 JRDevo."
                   HELP_LI L"Your recordings are yours: you may publish them and earn money from them (ads, sponsors, streams, paid content)."
                   HELP_LI L"Do not sell the tool or put it in a paid product or service without written permission."},
};
#undef HELP_BR
#undef HELP_LI

const wchar_t* HelpText(int id) {
    for (const Help& h : kHelp) if (h.id == id) return h.text;
    return nullptr;
}

// Help text for the control under the mouse / with the focus; nullptr = none (the overview is shown).
// Labels have id -1 and carry the id of the control they describe in GWLP_USERDATA.
const wchar_t* HelpForWindow(HWND h) {
    if (!h || h == g_dlg) return nullptr;
    int id = GetDlgCtrlID(h);
    if (id == -1) id = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
    return id > 0 ? HelpText(id) : nullptr;
}

void ShowHelp(const wchar_t* text) {
    if (!text) text = kOverview;
    if (text == g_shown || !g_dlg) return;
    g_shown = text;
    SetWindowTextW(GetDlgItem(g_dlg, ID_HELPTEXT), text);
}

void Hover(HWND h) {
    if (h == g_hover) return;
    g_hover = h;
    if (h && h == GetDlgItem(g_dlg, ID_HELPTEXT)) return;   // let the user read / scroll the panel
    ShowHelp(HelpForWindow(h));
}

void AddTip(HWND ctl, const wchar_t* text) {
    if (!g_tip || !ctl || !text) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_dlg;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

HWND Ctl(int id) { return GetDlgItem(g_dlg, id); }

std::wstring GetText(int id) {
    wchar_t b[1024];
    GetWindowTextW(Ctl(id), b, 1024);
    return b;
}

// ---- tabs (g_tab, g_madeTab, g_wantHidden)
int TabOf(HWND c) {
    for (size_t i = 0; i < g_made.size() && i < g_madeTab.size(); ++i)
        if (g_made[i] == c) return g_madeTab[i];
    return -1;
}
bool WantHidden(HWND c) { return std::find(g_wantHidden.begin(), g_wantHidden.end(), c) != g_wantHidden.end(); }
// Shows / hides a control on its own account (its tab still decides whether it is on screen).
void SetShown(HWND c, bool show) {
    if (!c) return;
    const auto it = std::find(g_wantHidden.begin(), g_wantHidden.end(), c);
    if (show && it != g_wantHidden.end()) g_wantHidden.erase(it);
    if (!show && it == g_wantHidden.end()) g_wantHidden.push_back(c);
    const int t = TabOf(c);
    const bool vis = show && (t < 0 || t == g_tab);
    if (((GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) != 0) != vis) ShowWindow(c, vis ? SW_SHOWNA : SW_HIDE);
}
// Shows the controls of g_tab and hides the others; the keyboard focus leaves a control that is now hidden.
void ApplyTab() {
    if (!g_dlg) return;
    for (size_t i = 0; i < g_made.size() && i < g_madeTab.size(); ++i) {
        HWND c = g_made[i];
        const bool vis = (g_madeTab[i] < 0 || g_madeTab[i] == g_tab) && !WantHidden(c);
        if (((GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) != 0) != vis) ShowWindow(c, vis ? SW_SHOWNA : SW_HIDE);
    }
    HWND f = GetFocus();
    if (f && IsChild(g_dlg, f) && !IsWindowVisible(f) && IsWindowVisible(g_dlg)) SetFocus(Ctl(ID_TABS));
    RedrawWindow(g_dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
void SetTab(int t) {
    t = (t % kTabCount + kTabCount) % kTabCount;
    if (t == g_tab) return;
    g_tab = t;
    ApplyTab();
}

// The tab strip (ID_TABS, owner-drawn): a rounded track across the content width, split into kTabCount equal segments. The
// selected segment is a raised pill (a card colour, a hairline edge and a soft shadow line) with a short accent bar under its
// text, like the Windows 11 selector bar; the hovered one gets a light tint. Shapes in GDI+ (anti-aliased, in layout units
// through a scale transform), text in GDI at the real size (sharp): the selected name semibold, the others regular.
RECT TabRect(const RECT& rc, int i) {
    const int w = rc.right - rc.left;
    return RECT{rc.left + w * i / kTabCount, rc.top, rc.left + w * (i + 1) / kTabCount, rc.bottom};
}
int TabAt(HWND strip, POINT pt) {
    RECT rc{};
    GetClientRect(strip, &rc);
    if (!PtInRect(&rc, pt)) return -1;
    for (int i = 0; i < kTabCount; ++i) {
        const RECT r = TabRect(rc, i);
        if (pt.x >= r.left && pt.x < r.right) return i;
    }
    return -1;
}
void DrawTabs(const DRAWITEMSTRUCT* di) {
    const RECT rc = di->rcItem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;   // device pixels
    if (w <= 0 || h <= 0) return;
    const glass::Theme& th = glass::Current();
    using Gdiplus::Color;
    auto argb = [](COLORREF c, BYTE a) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); };
    auto round = [](Gdiplus::GraphicsPath& p, float x, float y, float rw, float rh, float r) {
        const float d = 2 * r;
        p.AddArc(x, y, d, d, 180, 90);
        p.AddArc(x + rw - d, y, d, d, 270, 90);
        p.AddArc(x + rw - d, y + rh - d, d, d, 0, 90);
        p.AddArc(x, y + rh - d, d, d, 90, 90);
        p.CloseFigure();
    };
    {
        HBRUSH bg = CreateSolidBrush((COLORREF)di->itemData);   // glass: black, then white (alpha from the two passes)
        FillRect(di->hDC, &rc, bg);
        DeleteObject(bg);
    }
    const bool focused = GetFocus() == di->hwndItem;
    const float k = (float)g_s, lw = w / k, lh = h / k;   // layout units
    const float pad = 4, barW = 20, barH = 3;
    {
        Gdiplus::Graphics g(di->hDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.ScaleTransform(k, k);
        // the track: the colours of a glass card
        {
            Gdiplus::GraphicsPath p;
            round(p, 0.5f, 0.5f, lw - 1, lh - 1, 8);
            Gdiplus::SolidBrush b(th.dark ? Color(0x0D, 255, 255, 255) : Color(0xB3, 255, 255, 255));
            g.FillPath(&b, &p);
            Gdiplus::Pen pen(th.dark ? Color(0x26, 0, 0, 0) : Color(0x12, 0, 0, 0), 1.f);
            g.DrawPath(&pen, &p);
        }
        for (int i = 0; i < kTabCount; ++i) {
            const float x0 = lw * i / kTabCount, x1 = lw * (i + 1) / kTabCount;
            const float x = x0 + pad, y = pad, pw = x1 - x0 - 2 * pad, ph = lh - 2 * pad;
            if (i == g_tab) {
                // shadow line under the pill, the pill, its hairline edge
                {
                    Gdiplus::GraphicsPath sp;
                    round(sp, x, y + 1, pw, ph, 6);
                    Gdiplus::SolidBrush sb(th.dark ? Color(0x40, 0, 0, 0) : Color(0x14, 0, 0, 0));
                    g.FillPath(&sb, &sp);
                }
                Gdiplus::GraphicsPath p;
                round(p, x, y, pw, ph, 6);
                Gdiplus::SolidBrush fill(th.dark ? Color(0x1F, 255, 255, 255) : Color(0xFF, 255, 255, 255));
                g.FillPath(&fill, &p);
                Gdiplus::Pen edge(th.dark ? Color(0x17, 255, 255, 255) : Color(0x0F, 0, 0, 0), 1.f);
                g.DrawPath(&edge, &p);
                if (focused) {   // keyboard focus: the Windows 11 focus ring around the selected pill
                    Gdiplus::GraphicsPath fp;
                    round(fp, x - 2, y - 2, pw + 4, ph + 4, 8);
                    Gdiplus::Pen fr(argb(th.text, 0xFF), 2.f);
                    g.DrawPath(&fr, &fp);
                }
                Gdiplus::GraphicsPath bar;
                round(bar, x + (pw - barW) / 2, y + ph - barH - 3, barW, barH, barH / 2);
                Gdiplus::SolidBrush ab(argb(th.accent, 0xFF));
                g.FillPath(&ab, &bar);
            } else if (i == g_tabHover) {
                Gdiplus::GraphicsPath p;
                round(p, x, y, pw, ph, 6);
                Gdiplus::SolidBrush b(th.dark ? Color(0x0F, 255, 255, 255) : Color(0x0A, 0, 0, 0));
                g.FillPath(&b, &p);
            }
        }
    }
    // icon + name, centred in each segment and a little above the middle (room for the accent bar); grey smoothing: the glass
    // two-pass alpha needs the same text pixels on black and on white
    SetBkMode(di->hDC, TRANSPARENT);
    const int textBottom = h - Px(3);
    for (int i = 0; i < kTabCount; ++i) {
        const RECT r = TabRect(RECT{0, 0, w, h}, i);
        const bool sel = i == g_tab, hot = i == g_tabHover;
        HFONT nameFont = sel && g_tabFont ? g_tabFont : g_odFont;
        HGDIOBJ old = SelectObject(di->hDC, nameFont);
        SIZE ns{};
        GetTextExtentPoint32W(di->hDC, kTabNames[i], (int)wcslen(kTabNames[i]), &ns);
        SIZE is{};
        if (g_iconFont) {
            SelectObject(di->hDC, g_iconFont);
            GetTextExtentPoint32W(di->hDC, kTabIcons[i], 1, &is);
        }
        const int gap = g_iconFont ? Px(8) : 0, total = is.cx + gap + ns.cx;
        int x = r.left + ((r.right - r.left) - total) / 2;
        if (g_iconFont) {
            ::SetTextColor(di->hDC, sel ? th.accent : hot ? th.text : th.textDim);
            RECT ir{x, 0, x + is.cx + 2, textBottom};
            DrawTextW(di->hDC, kTabIcons[i], 1, &ir, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            x += is.cx + gap;
        }
        SelectObject(di->hDC, nameFont);
        ::SetTextColor(di->hDC, sel || hot ? th.text : th.textDim);
        RECT tr{x, 0, x + ns.cx + 2, textBottom};
        DrawTextW(di->hDC, kTabNames[i], -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(di->hDC, old);
    }
}
// Mouse and keyboard on the tab strip: click / arrow keys / Home / End pick a tab; the hover tint follows the mouse.
LRESULT CALLBACK TabsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        const int t = TabAt(h, POINT{(short)LOWORD(lp), (short)HIWORD(lp)});
        SetFocus(h);
        if (t >= 0) SetTab(t);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int t = TabAt(h, POINT{(short)LOWORD(lp), (short)HIWORD(lp)});
        if (t != g_tabHover) {
            g_tabHover = t;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        if (g_tabHover >= 0) {
            g_tabHover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (wp == VK_LEFT || wp == VK_UP) { SetTab(g_tab - 1); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (wp == VK_RIGHT || wp == VK_DOWN) { SetTab(g_tab + 1); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (wp == VK_HOME) { SetTab(0); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (wp == VK_END) { SetTab(kTabCount - 1); InvalidateRect(h, nullptr, FALSE); return 0; }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// "Make for YouTube" drop-down <-> the two hidden checks bound to post_mkv / post_yt8k.
// 0 = 3D .mkv (mkv 1, 8k 0), 1 = 8K 3D .mkv (0, 1), 2 = both files (1, 1), 3 = nothing (0, 0)
HWND g_layoutLabel = nullptr;   // label of the 3D layout drop-down (greyed together with it)
std::vector<HWND> g_domeLabels;  // "Dome fill:", "Fade after:", "s": greyed with the dome fields (UpdateLayoutEnabled)
// The 3D layout and "Record directly in the YouTube layout" only apply to flat recordings: VR180 / VR360 (projection items 1
// and 2) have fixed layouts. The other way round, Dome fill / Fade after / Shared view only apply to VR180 / VR360. Called on CBN_SELCHANGE of
// ID_PROJECTION and from SetControls.
void UpdateLayoutEnabled() {
    const bool flat = (int)SendMessageW(Ctl(ID_PROJECTION), CB_GETCURSEL, 0, 0) <= 0;
    EnableWindow(Ctl(ID_LAYOUT3D), flat);
    if (g_layoutLabel) EnableWindow(g_layoutLabel, flat);
    EnableWindow(Ctl(ID_RECORDYT), flat);
    EnableWindow(Ctl(ID_FILL), !flat);
    EnableWindow(Ctl(ID_FILLFADE), !flat);
    EnableWindow(Ctl(ID_SHAREDVIEW), !flat);
    for (HWND l : g_domeLabels) EnableWindow(l, !flat);
}
// record_for_youtube is ticked (and the projection is flat: dome projections ignore the key)
bool RecordForYoutubeOn() {
    return SendMessageW(Ctl(ID_RECORDYT), BM_GETCHECK, 0, 0) == BST_CHECKED &&
           (int)SendMessageW(Ctl(ID_PROJECTION), CB_GETCURSEL, 0, 0) <= 0;
}
void YtModeFromChecks() {
    const bool mkv = SendMessageW(Ctl(ID_POSTMKV), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool k8 = SendMessageW(Ctl(ID_POSTYT8K), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int sel = mkv ? (k8 ? 2 : 0) : (k8 ? 1 : 3);
    SendMessageW(Ctl(ID_YTMODE), CB_SETCURSEL, (WPARAM)sel, 0);
}
void ChecksFromYtMode() {
    const int sel = (int)SendMessageW(Ctl(ID_YTMODE), CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 3) return;   // no selection: keep the checks as they are
    SendMessageW(Ctl(ID_POSTMKV), BM_SETCHECK, (sel == 0 || sel == 2) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Ctl(ID_POSTYT8K), BM_SETCHECK, (sel == 1 || sel == 2) ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ---- live preview of the indicator dot (ID_INDPREVIEW, owner-drawn)
// The circle is the headset's field of view, straight ahead in the middle. The dot sits at the angles atan2(x, -z) / atan2(y, -z),
// mapped linearly to the radius (kPreviewHalfFovDeg = the edge). The values come from the edits: text that does not parse keeps the last good value.
const double kPreviewHalfFovDeg = 50.0;   // about +-50 degrees horizontally and vertically
const UINT_PTR kPulseTimer = 4;           // 50 ms: the preview pulses like the real indicator (1 Hz)
struct IndPreview { bool on = true; double x = 0.22, y = -0.16, z = -0.8, size = 0.02, opacity = 1.0; };
IndPreview g_prev;

bool ParseNumber(int id, double& out) {
    const std::wstring t = Trim(GetText(id));
    if (t.empty()) return false;
    wchar_t* end = nullptr;
    const double v = wcstod(t.c_str(), &end);
    if (!end || *end != L'\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

void UpdatePreview() {
    if (!g_dlg || !Ctl(ID_INDPREVIEW)) return;
    double v;
    g_prev.on = SendMessageW(Ctl(ID_INDICATOR), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ParseNumber(ID_INDX, v)) g_prev.x = v;
    if (ParseNumber(ID_INDY, v)) g_prev.y = v;
    if (ParseNumber(ID_INDZ, v)) g_prev.z = v;
    if (ParseNumber(ID_INDSIZE, v) && v > 0) g_prev.size = v;
    if (ParseNumber(ID_INDOPACITY, v)) g_prev.opacity = v < 0 ? 0 : (v > 1 ? 1 : v);
    InvalidateRect(Ctl(ID_INDPREVIEW), nullptr, FALSE);
}

COLORREF Mix(COLORREF bg, COLORREF fg, double a) {
    auto m = [a](int b, int f) { return (int)(b + (f - b) * a + 0.5); };
    return RGB(m(GetRValue(bg), GetRValue(fg)), m(GetGValue(bg), GetGValue(fg)), m(GetBValue(bg), GetBValue(fg)));
}

void DrawPreview(const DRAWITEMSTRUCT* di) {
    const RECT rc = di->rcItem;
    const int pw = rc.right - rc.left, ph = rc.bottom - rc.top;   // device pixels
    const int w = (int)(pw / g_s), h = (int)(ph / g_s);           // layout units (the world transform scales)
    const double kPi = 3.14159265358979323846;
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, pw, ph);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const bool on = g_prev.on;
    // the window's card shows through: bg = the colour glass asks for (black, then white: alpha from the two passes)
    const bool dark = glass::Current().dark;
    const COLORREF bg = (COLORREF)di->itemData;
    const COLORREF edge = on ? (dark ? RGB(140, 146, 162) : RGB(120, 126, 140)) : (dark ? RGB(95, 95, 95) : RGB(170, 170, 170));
    const COLORREF cross = dark ? RGB(70, 72, 80) : RGB(205, 208, 215), txt = glass::Current().textDim;
    HBRUSH bgBrush = CreateSolidBrush(bg);
    const RECT all{0, 0, pw, ph};
    FillRect(dc, &all, bgBrush);
    DeleteObject(bgBrush);
    SetGraphicsMode(dc, GM_ADVANCED);
    const XFORM xf{(FLOAT)g_s, 0, 0, (FLOAT)g_s, 0, 0};
    SetWorldTransform(dc, &xf);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = g_odFontLg ? SelectObject(dc, g_odFontLg) : nullptr;

    const int cx = w / 2, cy = h / 2, R = (w < h ? w : h) / 2 - 4;
    const double pxPerDeg = R / kPreviewHalfFovDeg;
    HPEN crossPen = CreatePen(PS_SOLID, 1, cross);
    HGDIOBJ oldPen = SelectObject(dc, crossPen);
    MoveToEx(dc, cx - R, cy, nullptr); LineTo(dc, cx + R + 1, cy);
    MoveToEx(dc, cx, cy - R, nullptr); LineTo(dc, cx, cy + R + 1);
    HPEN edgePen = CreatePen(PS_SOLID, 2, edge);
    SelectObject(dc, edgePen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - R, cy - R, cx + R + 1, cy + R + 1);

    const bool behind = g_prev.z >= 0;
    if (!behind) {
        const double d = -g_prev.z, rad2deg = 180.0 / kPi;
        const double ax = std::atan2(g_prev.x, d) * rad2deg, ay = std::atan2(g_prev.y, d) * rad2deg;
        double dx = ax * pxPerDeg, dy = -ay * pxPerDeg;
        const double len = std::sqrt(dx * dx + dy * dy);
        const bool outside = len > R;
        if (outside) { dx *= R / len; dy *= R / len; }   // outside the view: parked on the edge, fainter
        double dotR = std::atan(g_prev.size / 2 / d) * rad2deg * pxPerDeg;   // half of the angular size 2*atan(size/2/d), in px
        if (dotR < 3) dotR = 3;
        double a = 1.0;
        if (on) {
            const double s = 0.5 + 0.5 * std::sin((double)(GetTickCount() % 1000) / 1000.0 * 2 * kPi);
            a = g_prev.opacity * (0.35 + 0.65 * s);
            if (outside) a *= 0.5;
        }
        const COLORREF col = on ? Mix(bg, RGB(235, 40, 40), a) : Mix(bg, RGB(128, 128, 128), 1.0);
        HBRUSH dotBrush = CreateSolidBrush(col);
        HPEN none = CreatePen(PS_NULL, 0, 0);
        SelectObject(dc, dotBrush);
        SelectObject(dc, none);
        const int px = cx + (int)std::lround(dx), py = cy + (int)std::lround(dy), r = (int)std::lround(dotR);
        Ellipse(dc, px - r, py - r, px + r + 1, py + r + 1);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        SelectObject(dc, edgePen);
        DeleteObject(dotBrush);
        DeleteObject(none);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(crossPen);
    DeleteObject(edgePen);

    SetTextColor(dc, txt);
    const wchar_t* note = !on ? L"off" : (behind ? L"behind you" : nullptr);
    if (note) {   // below the centre, clear of the ring
        RECT t2{0, cy + 8, w, cy + 24};
        DrawTextW(dc, note, -1, &t2, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (oldFont) SelectObject(dc, oldFont);
    ModifyWorldTransform(dc, nullptr, MWT_IDENTITY);
    BitBlt(di->hDC, rc.left, rc.top, pw, ph, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// Mouse on the preview (subclassed in the Create pass): click or drag = the dot goes there (X / Y fields follow; the distance
// Z is kept), the wheel = nearer / farther (Z, 5 cm a notch; the angles are kept). The inverse of DrawPreview's mapping,
// clamped to the view circle and to the config ranges (x / y -2..2, z -10..-0.05). The fields' EN_CHANGE repaints.
void PreviewCircle(HWND h, double& cx, double& cy, double& R) {
    RECT rc{};
    GetClientRect(h, &rc);
    const int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    cx = w / 2; cy = hh / 2; R = (w < hh ? w : hh) / 2 - Px(4);   // device pixels: DrawPreview's circle
}

void SetFieldNumber(int id, double v) {
    wchar_t b[32];
    swprintf_s(b, L"%.2f", std::fabs(v) < 0.005 ? 0.0 : v);   // no "-0.00"
    SetWindowTextW(Ctl(id), b);
}

void PreviewPlace(HWND h, int mx, int my) {
    double cx, cy, R;
    PreviewCircle(h, cx, cy, R);
    if (R <= 0) return;
    double dx = mx - cx, dy = my - cy;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len > R) { dx *= R / len; dy *= R / len; }
    const double radPerPx = kPreviewHalfFovDeg / R * 3.14159265358979323846 / 180.0;
    const bool newZ = g_prev.z > -0.05;   // behind you / at the eye: put it at the default distance first
    const double d = newZ ? 0.8 : -g_prev.z;
    auto clamp2 = [](double v) { return v < -2.0 ? -2.0 : (v > 2.0 ? 2.0 : v); };
    SetFieldNumber(ID_INDX, clamp2(std::tan(dx * radPerPx) * d));
    SetFieldNumber(ID_INDY, clamp2(std::tan(-dy * radPerPx) * d));
    if (newZ) SetFieldNumber(ID_INDZ, -d);
}

void PreviewWheel(int delta) {
    if (g_prev.z > -0.05) return;
    const double d0 = -g_prev.z;
    double d1 = d0 - 0.05 * delta / WHEEL_DELTA;   // wheel up = nearer
    d1 = d1 < 0.05 ? 0.05 : (d1 > 10.0 ? 10.0 : d1);
    auto clamp2 = [](double v) { return v < -2.0 ? -2.0 : (v > 2.0 ? 2.0 : v); };
    const double k = d1 / d0;   // same angles: x and y scale with the distance
    SetFieldNumber(ID_INDX, clamp2(g_prev.x * k));
    SetFieldNumber(ID_INDY, clamp2(g_prev.y * k));
    SetFieldNumber(ID_INDZ, -d1);
}

LRESULT CALLBACK PreviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(h);
        PreviewPlace(h, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == h && (wp & MK_LBUTTON)) PreviewPlace(h, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, PreviewProc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ---- live preview of the 3D layout (ID_LAYOUTPREVIEW, owner-drawn): the sizes come from pp::PredictLayout, the same arithmetic the
// conversion uses. It reads the controls at paint time, so it is always current; CommandHandler / SetControls only invalidate it.
const int kExampleSbsW = 8192, kExampleSbsH = 4630;   // a Quest 3 recording (eyes 4096x4630), the example the preview starts from

std::wstring ComboValue(int id) {   // the config value of the selected item of a combo bound to a field ("" = none)
    for (const Field& f : kFields) {
        if (f.id != id) continue;
        const int sel = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0);
        const std::wstring opts = f.options;
        int idx = 0;
        size_t pos = 0;
        while (pos <= opts.size()) {
            size_t bar = opts.find(L'|', pos);
            if (bar == std::wstring::npos) bar = opts.size();
            if (idx == sel) return opts.substr(pos, bar - pos);
            ++idx;
            pos = bar + 1;
        }
        if (sel == idx && f.custom)   // the "Custom (...)" item
            for (const auto& cv : g_customValue) if (cv.first == id) return cv.second;
    }
    return L"";
}

// "about 54 GB per hour" next to the bitrate drop-down (Default = 120 Mbit/s HEVC / 80 H.264 / 80 AV1, per the Codec drop-down).
// 1 Mbit/s = 3600 / 8 / 1000 = 0.45 GB per hour (decimal GB, like the bitrate itself).
void UpdateBitrateHint() {
    const std::wstring v = ComboValue(ID_BITRATE);
    unsigned mbps = (unsigned)wcstoul(v.c_str(), nullptr, 10);
    const bool def = mbps == 0;
    const std::wstring codec = ComboValue(ID_CODEC);
    const VideoCodec vc = codec == L"h264" ? VideoCodec::H264 : codec == L"av1" ? VideoCodec::Av1 : VideoCodec::Hevc;
    if (def) mbps = VideoCodecDefaultMbps(vc);
    {   // item 0 names the codec's default ("Default (120 Mbit/s)" HEVC, 80 H.264 / AV1); the selection is kept
        const HWND h = Ctl(ID_BITRATE);
        wchar_t want[48], cur[64] = L"";
        swprintf_s(want, L"Default (%u Mbit/s)", VideoCodecDefaultMbps(vc));
        const bool filled = h && SendMessageW(h, CB_GETCOUNT, 0, 0) > 0;
        if (filled && SendMessageW(h, CB_GETLBTEXTLEN, 0, 0) < 64) SendMessageW(h, CB_GETLBTEXT, 0, (LPARAM)cur);
        if (filled && wcscmp(cur, want) != 0) {
            const int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
            SendMessageW(h, CB_DELETESTRING, 0, 0);
            SendMessageW(h, CB_INSERTSTRING, 0, (LPARAM)want);
            SendMessageW(h, CB_SETCURSEL, (WPARAM)sel, 0);
        }
    }
    wchar_t b[96];
    swprintf_s(b, L"about %u GB per hour%s", (mbps * 45u + 50u) / 100u,
               !def || vc == VideoCodec::Hevc ? L"" : vc == VideoCodec::Av1 ? L" (AV1: 80 Mbit/s)" : L" (H.264: 80 Mbit/s)");
    SetWindowTextW(Ctl(ID_BITRATEHINT), b);
}

// The 8K bitrate and 8K codec only matter when "YouTube output" makes the 8K file (items 1 and 2) of a flat recording: the
// 8K step is skipped for VR180 / VR360 (projection items 1 and 2) and for recordings made in the YouTube layout
// (record_for_youtube: the recording already is the 8K file). Their labels are greyed with them.
// Rate control: the QP edit (+ its "QP:" label and hint) only applies to Constant QP; the bitrate drop-down (+ its label and the
// size hint) does not apply to it. Called on CBN_SELCHANGE of ID_RATECONTROL and from SetControls.
HWND g_bitrateLabel = nullptr;
std::vector<HWND> g_qpLabels;
void UpdateRateControlEnabled() {
    const bool qp = ComboValue(ID_RATECONTROL) == L"qp";
    EnableWindow(Ctl(ID_QP), qp);
    for (HWND l : g_qpLabels) EnableWindow(l, qp);
    EnableWindow(Ctl(ID_BITRATE), !qp);
    EnableWindow(Ctl(ID_BITRATEHINT), !qp);
    if (g_bitrateLabel) EnableWindow(g_bitrateLabel, !qp);
}

HWND g_yt8kBitrateLabel = nullptr, g_yt8kCodecLabel = nullptr;
void UpdateYt8kEnabled() {
    const int sel = (int)SendMessageW(Ctl(ID_YTMODE), CB_GETCURSEL, 0, 0);
    const bool flat = (int)SendMessageW(Ctl(ID_PROJECTION), CB_GETCURSEL, 0, 0) <= 0;
    const bool on = flat && (sel == 1 || sel == 2) && !RecordForYoutubeOn();
    EnableWindow(Ctl(ID_YT8KBITRATE), on);
    EnableWindow(Ctl(ID_YT8KCODEC), on);
    if (g_yt8kBitrateLabel) EnableWindow(g_yt8kBitrateLabel, on);
    if (g_yt8kCodecLabel) EnableWindow(g_yt8kCodecLabel, on);
}

// "E: 812 GB free" under the output folder (src/disk.h); an empty field = the default captures folder next to the app.
// Typing in the field re-arms kFreeSpaceTimer (one-shot): the drive is queried once the typing pauses, not per key.
const UINT_PTR kFreeSpaceTimer = 6;
const UINT     kFreeSpaceDelayMs = 400;
void UpdateFreeSpace() {
    if (g_dlg) KillTimer(g_dlg, kFreeSpaceTimer);   // a pending debounced refresh is done by this call
    std::wstring dir = Trim(GetText(ID_OUTDIR));
    const bool def = dir.empty();
    if (def) dir = DefaultCapturesDir(ExeFolder());
    // one query (an offline network drive can take seconds per GetDiskFreeSpaceExW); the text is disk::FreeText's
    unsigned long long f = 0;
    const bool known = disk::FreeBytes(dir, f);
    const std::wstring drive = disk::DriveName(dir);
    std::wstring t = !known ? std::wstring(L"free space unknown")
                            : (drive.empty() ? std::wstring() : drive + L" ") + disk::SizeText(f) + L" free";
    if (known && f < disk::MinFreeToStart())
        t += L" -- below " + disk::SizeText(disk::MinFreeToStart()) + L": recordings and conversions will not start";
    else if (def) t += L"  (the captures folder next to the app)";
    SetWindowTextW(Ctl(ID_OUTFREE), t.c_str());
}

// The red line under the Export settings: shown only while a game runs at a frame rate that 60 does not divide evenly
// (pp::UnevenFor60); refreshed by kQueueTimer / WM_QUEUE_POKE. The text is set before the window is shown / hidden.
void UpdateFpsWarning() {
    HWND c = g_dlg ? Ctl(ID_FPSWARN) : nullptr;
    if (!c) return;
    const double fps = g_hooks.gameFps ? (double)g_hooks.gameFps() : 0.0;
    std::wstring t = pp::FpsWarningText(fps);
    RECT rc{};
    GetClientRect(c, &rc);
    if (!t.empty()) {   // the long wording when it fits the label, else the short one
        HDC dc = GetDC(c);
        HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
        SIZE z{};
        GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &z);
        if (old) SelectObject(dc, old);
        ReleaseDC(c, dc);
        if (z.cx > rc.right) t = pp::FpsWarningText(fps, true);
    }
    wchar_t cur[256] = L"";
    GetWindowTextW(c, cur, 256);
    if (t != cur) SetWindowTextW(c, t.c_str());
    SetShown(c, !t.empty());
}

void UpdateLayoutPreview() {
    if (g_dlg && Ctl(ID_LAYOUTPREVIEW)) InvalidateRect(Ctl(ID_LAYOUTPREVIEW), nullptr, FALSE);
}

// The "update available" SysLink at the right end of the header and the "Check now" button (App tab). Reads the tray's update
// state through g_update (no hook = no updater, e.g. settings_test: the line stays hidden and the button is greyed).
void UpdateUpdateLine() {
    if (!g_dlg) return;
    HWND link = Ctl(ID_UPDATELINK);
    HWND btn = Ctl(ID_UPDATENOW);
    const UpdateState s = g_update.state ? g_update.state() : UpdateState();
    if (btn) EnableWindow(btn, g_update.checkNow && !s.downloading);
    if (link) {
        std::wstring t;
        if (s.downloading) t = L"Downloading the update...";
        else if (s.available) t = L"VR EyeCapture " + s.versionText + L" is available.  <a>Update now</a>";
        const bool show = !t.empty() && (bool)g_update.startUpdate;
        wchar_t cur[256] = L"";
        GetWindowTextW(link, cur, 256);
        if (t != cur) SetWindowTextW(link, t.c_str());
        SetShown(link, show);
    }
}

// ---- the logo at the top of the window (ID_LOGO, owner-drawn): the mark of scripts\make_logo.py (VR goggles, a 256-unit design box),
// the wordmark in bold and the tagline in grey. The mark is drawn SS times bigger into a memory bitmap and StretchBlt'ed down
// with HALFTONE (GDI has no anti-aliasing); the words are drawn straight onto the item at the real size, so they stay sharp.
// The layout (logoW) in BuildControls must match the x positions used here: the mark is the square (0, 0, h, h), then 6, the
// wordmark, 16, the tagline (layout units).
void DrawLogo(const DRAWITEMSTRUCT* di) {
    const int SS = 4;
    const RECT rc = di->rcItem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    const int H = h * SS;
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, H, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const RECT all{0, 0, H, H};
    HBRUSH bgBrush = CreateSolidBrush((COLORREF)di->itemData);   // glass: black, then white (alpha from the two passes)
    FillRect(dc, &all, bgBrush);
    DeleteObject(bgBrush);

    const double s = (double)H / 256.0;
    auto U = [&](double v) { return (int)(v * s + 0.5); };
    const COLORREF grey = RGB(154, 163, 178), blue = RGB(59, 139, 255), coral = RGB(255, 107, 107), red = RGB(255, 59, 59);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    auto disc = [&](double cx, double cy, double r, COLORREF c) {   // filled circle, no outline
        HBRUSH br = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, br);
        SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, U(cx - r), U(cy - r), U(cx + r) + 1, U(cy + r) + 1);
        SelectObject(dc, ob);
        DeleteObject(br);
    };
    // head strap stubs: round-ended lines
    {
        LOGBRUSH lb{BS_SOLID, grey, 0};
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)(U(9) > 1 ? U(9) : 1), &lb, 0, nullptr);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, U(4), U(128), nullptr);
        LineTo(dc, U(24), U(128));
        MoveToEx(dc, U(232), U(128), nullptr);
        LineTo(dc, U(252), U(128));
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    // bridge between the lenses
    {
        HBRUSH br = CreateSolidBrush(grey);
        HGDIOBJ ob = SelectObject(dc, br);
        SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, U(112), U(117), U(144) + 1, U(139) + 1, U(12), U(12));
        SelectObject(dc, ob);
        DeleteObject(br);
    }
    // lenses: an outline of 11 units that stays inside the design box (the rect is inset by half the pen width, and so is the
    // corner: outer corner diameter 52 -> 52 - 11 on the pen's centre line)
    {
        const int pw = U(11) > 1 ? U(11) : 1;
        const int ins = pw / 2;
        auto lens = [&](double l, double t, double r, double b, COLORREF c) {
            HPEN pen = CreatePen(PS_SOLID, pw, c);
            HGDIOBJ op = SelectObject(dc, pen);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, U(l) + ins, U(t) + ins, U(r) - ins, U(b) - ins, U(52 - 11), U(52 - 11));
            SelectObject(dc, op);
            DeleteObject(pen);
        };
        lens(24, 76, 118, 180, blue);
        lens(138, 76, 232, 180, coral);
    }
    // pupils, looking slightly inwards
    disc(78, 128, 17, blue);
    disc(178, 128, 17, coral);
    // recording dot
    disc(222, 50, 18, RGB(255, 255, 255));
    disc(222, 50, 13, red);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SetStretchBltMode(di->hDC, HALFTONE);
    SetBrushOrgEx(di->hDC, 0, 0, nullptr);
    StretchBlt(di->hDC, rc.left, rc.top, h, h, dc, 0, 0, H, H, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);

    // wordmark (bold) and tagline (grey), vertically centred on the mark; grey smoothing: the glass two-pass alpha needs the
    // same text pixels on black and on white
    SetBkMode(di->hDC, TRANSPARENT);
    int x = rc.left + h + Px(6);
    auto line = [&](HFONT f, const wchar_t* t, COLORREF c) {
        if (!f) return;
        HGDIOBJ of = SelectObject(di->hDC, f);
        SetTextColor(di->hDC, c);
        SIZE sz{};
        GetTextExtentPoint32W(di->hDC, t, (int)wcslen(t), &sz);
        RECT r{x, rc.top, x + sz.cx + 2, rc.bottom};
        DrawTextW(di->hDC, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        x += sz.cx;
        SelectObject(di->hDC, of);
    };
    line(g_logoFont, L"VR EyeCapture", glass::Current().text);
    x += Px(16);
    line(g_odFont, L"Stereo 3D recorder for VR games", glass::Current().textDim);
}

void DrawLayoutPreview(const DRAWITEMSTRUCT* di) {
    const RECT rc = di->rcItem;
    const int pw = rc.right - rc.left, ph = rc.bottom - rc.top;   // device pixels
    const int w = (int)(pw / g_s);                                // layout units (the world transform scales)
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, pw, ph);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const bool dark = glass::Current().dark;
    const COLORREF bg = (COLORREF)di->itemData;   // the card shows through (glass two-pass alpha)
    const COLORREF edge = dark ? RGB(150, 158, 180) : RGB(110, 116, 130), txt = glass::Current().text, dim = glass::Current().textDim;
    const COLORREF colL = RGB(56, 88, 140), colR = RGB(140, 66, 66);
    const RECT all{0, 0, pw, ph};
    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(dc, &all, bgBrush);
    DeleteObject(bgBrush);
    SetGraphicsMode(dc, GM_ADVANCED);
    const XFORM xf{(FLOAT)g_s, 0, 0, (FLOAT)g_s, 0, 0};
    SetWorldTransform(dc, &xf);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = g_odFontLg ? SelectObject(dc, g_odFontLg) : nullptr;

    auto text = [&](int x, int y, int x2, const wchar_t* t, COLORREF c, UINT fmt = DT_LEFT) {
        SetTextColor(dc, c);
        RECT r{x, y, x2, y + 15};
        DrawTextW(dc, t, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    };
    auto fillRect = [&](int l, int t, int r, int b, COLORREF c) {
        HBRUSH br = CreateSolidBrush(c);
        const RECT q{l, t, r, b};
        FillRect(dc, &q, br);
        DeleteObject(br);
    };
    auto frameRect = [&](int l, int t, int r, int b, COLORREF c) {
        HBRUSH br = CreateSolidBrush(c);
        const RECT q{l, t, r, b};
        FrameRect(dc, &q, br);
        DeleteObject(br);
    };

    const bool dome = (int)SendMessageW(Ctl(ID_PROJECTION), CB_GETCURSEL, 0, 0) > 0;
    const std::wstring lv = ComboValue(ID_LAYOUT3D), cv = ComboValue(ID_EYECROP), mv = ComboValue(ID_MAXWIDTH);
    const Layout3d lay = lv == L"sbs_half" ? Layout3d::SbsHalf : lv == L"sbs_full" ? Layout3d::SbsFull
                         : lv == L"tb_full" ? Layout3d::TbFull : Layout3d::TbHalf;
    const EyeCrop crop = pp::CropFromName(elog::Narrow(cv));
    const wchar_t* cropShown = crop == EyeCrop::Wide169 ? L"16:9" : crop == EyeCrop::Pad43 ? L"4:3 in 16:9" : L"4:3";
    const int ytSel = (int)SendMessageW(Ctl(ID_YTMODE), CB_GETCURSEL, 0, 0);   // 0 mkv, 1 8K, 2 both, 3 nothing
    const bool wantMkv = ytSel == 0 || ytSel == 2, want8k = ytSel == 1 || ytSel == 2;
    // record_for_youtube: the layer records the YouTube frame itself (flat only); left = the headset's eyes, right = the recording
    const bool recYt = !dome && SendMessageW(Ctl(ID_RECORDYT), BM_GETCHECK, 0, 0) == BST_CHECKED;

    // the recording: the example scaled to the Resolution (the whole side-by-side width) and the codec's limit (H.264: 4096,
    // like src/recorder.cpp), even sizes. With recYt the left picture is the headset's native view instead.
    int recW = kExampleSbsW, recH = kExampleSbsH;
    const std::wstring codecV = ComboValue(ID_CODEC);
    const long codecMax = (long)VideoCodecMaxSide(codecV == L"h264" ? VideoCodec::H264 : codecV == L"av1" ? VideoCodec::Av1 : VideoCodec::Hevc);
    const long maxWSet = mv.empty() ? 0 : wcstol(mv.c_str(), nullptr, 10);   // max_width as set (0 = native)
    long maxW = maxWSet > 0 && maxWSet < codecMax ? maxWSet : codecMax;
    if (!recYt && maxW > 0 && maxW < kExampleSbsW) {
        recW = (int)(maxW & ~1L);
        recH = 2 * (int)std::floor(kExampleSbsH * ((double)recW / kExampleSbsW) / 2.0 + 0.5);
    }
    const int eyeW = recW / 2, eyeH = recH;
    const pp::LayoutSize nat = pp::PredictLayout(eyeW, eyeH, lay, crop, 0);
    const pp::LayoutSize big = pp::PredictLayout(eyeW, eyeH, lay, crop, 4320);
    const pp::LayoutSize& shown = want8k ? big : nat;
    const bool remux = lay == Layout3d::SbsFull && crop == EyeCrop::None;   // plain remux: the frame is the recording itself
    const bool plain = remux && !want8k;
    // recYt: the recorded frame = the 8K layout of the native eyes (short side 4320, long side <= 8192), then max_width (frame
    // width) and the codec limit, scaled uniformly and made even -- the same order as the recorder's FitOutputSize
    int ytW = big.fw, ytH = big.fh;
    if (recYt) {
        auto scaleTo = [&](long mw, long mh) {
            if (ytW > mw) { ytH = (int)((long long)ytH * mw / ytW); ytW = (int)mw; }
            if (ytH > mh) { ytW = (int)((long long)ytW * mh / ytH); ytH = (int)mh; }
        };
        if (maxWSet > 0) scaleTo(maxWSet, 16384);
        if (ytW > codecMax || ytH > codecMax) scaleTo(codecMax, codecMax);
        ytW &= ~1;
        ytH &= ~1;
    }

    // left: the recording, the part each eye loses to the crop hatched
    text(8, 3, 112, recYt ? L"Headset view" : L"Example recording", dim);
    const int sx = 8, sy = 20, sw = 84, sh = (int)std::lround(sw * (double)recH / recW);
    fillRect(sx, sy, sx + sw / 2, sy + sh, colL);
    fillRect(sx + sw / 2, sy, sx + sw, sy + sh, colR);
    if (!dome && nat.ch < eyeH) {
        const int cut = (int)std::lround((double)sh * (eyeH - nat.ch) / eyeH / 2.0);
        HBRUSH hb = CreateHatchBrush(HS_BDIAGONAL, RGB(210, 214, 224));
        HBRUSH dk = CreateSolidBrush(RGB(30, 32, 40));
        for (int e = 0; e < 2; ++e) {
            const int x0 = sx + e * (sw / 2), x1 = x0 + sw / 2;
            const RECT top{x0, sy, x1, sy + cut}, bot{x0, sy + sh - cut, x1, sy + sh};
            FillRect(dc, &top, dk); FillRect(dc, &bot, dk);
            FillRect(dc, &top, hb); FillRect(dc, &bot, hb);
        }
        DeleteObject(hb);
        DeleteObject(dk);
    }
    frameRect(sx, sy, sx + sw, sy + sh, edge);
    frameRect(sx + sw / 2, sy, sx + sw / 2 + 1, sy + sh, edge);
    text(sx, sy + sh / 2 - 7, sx + sw / 2, L"L", RGB(255, 255, 255), DT_CENTER);
    text(sx + sw / 2, sy + sh / 2 - 7, sx + sw, L"R", RGB(255, 255, 255), DT_CENTER);
    wchar_t buf[160];
    swprintf_s(buf, L"%dx%d", recW, recH);
    text(sx, sy + sh + 3, sx + sw + 12, buf, txt);

    // the arrow
    HPEN pen = CreatePen(PS_SOLID, 2, dim);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int ay = sy + sh / 2;
    MoveToEx(dc, 100, ay, nullptr); LineTo(dc, 118, ay);
    MoveToEx(dc, 113, ay - 4, nullptr); LineTo(dc, 119, ay); LineTo(dc, 113, ay + 5);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    // right: the output frame to aspect, L and R in the layout's arrangement
    const int ox = 126, oy = 20, boxW = w - ox - 6, boxH = 62;
    // a frame fw x fh to aspect, L / R side by side (sbs) or top / bottom; bar = the 4:3pad black bars on each side of an eye,
    // as a fraction of the eye's width (0 = none)
    auto drawFrame = [&](int fw, int fh, bool sbs, double bar) {
        double k = (double)boxW / fw;
        if ((double)boxH / fh < k) k = (double)boxH / fh;
        const int dw = (int)std::lround(fw * k), dh = (int)std::lround(fh * k);
        const RECT lr = sbs ? RECT{ox, oy, ox + dw / 2, oy + dh} : RECT{ox, oy, ox + dw, oy + dh / 2};
        const RECT rr = sbs ? RECT{ox + dw / 2, oy, ox + dw, oy + dh} : RECT{ox, oy + dh / 2, ox + dw, oy + dh};
        fillRect(lr.left, lr.top, lr.right, lr.bottom, colL);
        fillRect(rr.left, rr.top, rr.right, rr.bottom, colR);
        for (const RECT& e : {lr, rr}) {
            const int bw = (int)std::lround((e.right - e.left) * bar);
            if (bw <= 0) continue;
            fillRect(e.left, e.top, e.left + bw, e.bottom, RGB(0, 0, 0));
            fillRect(e.right - bw, e.top, e.right, e.bottom, RGB(0, 0, 0));
        }
        frameRect(ox, oy, ox + dw, oy + dh, edge);
        frameRect(rr.left, rr.top, rr.left + (sbs ? 1 : dw), rr.top + (sbs ? dh : 1), edge);
        text(lr.left, (lr.top + lr.bottom) / 2 - 7, lr.right, L"L", RGB(255, 255, 255), DT_CENTER);
        text(rr.left, (rr.top + rr.bottom) / 2 - 7, rr.right, L"R", RGB(255, 255, 255), DT_CENTER);
    };
    const bool laySbs = lay == Layout3d::SbsHalf || lay == Layout3d::SbsFull;
    if (dome) {   // the same picture, untouched
        fillRect(ox, oy, ox + boxW / 2, oy + sh, colL);
        fillRect(ox + boxW / 2, oy, ox + boxW, oy + sh, colR);
        frameRect(ox, oy, ox + boxW, oy + sh, edge);
        frameRect(ox + boxW / 2, oy, ox + boxW / 2 + 1, oy + sh, edge);
        text(ox, oy + sh / 2 - 7, ox + boxW / 2, L"L", RGB(255, 255, 255), DT_CENTER);
        text(ox + boxW / 2, oy + sh / 2 - 7, ox + boxW, L"R", RGB(255, 255, 255), DT_CENTER);
        text(ox, 3, w - 4, L"Copy", dim);
        text(8, 94, w - 4, L"Dome (VR180 / VR360): copied as is,", txt);
        text(8, 109, w - 4, L"layout and crop do not apply.", dim);
    } else if (recYt) {   // the recording IS the YouTube frame; the MKV step only copies it (no separate 8K file)
        drawFrame(ytW, ytH, laySbs, big.sw > 0 ? (double)big.vx / big.sw : 0.0);
        text(ox, 3, w - 4, L"Recording", dim);
        swprintf_s(buf, L"Recording: %dx%d %s", ytW, ytH, pp::LayoutName(lay));
        text(8, 94, w - 4, buf, txt);
        text(8, 109, w - 4, wantMkv ? L"(uploaded as is after a copy)" : L"(YouTube output None: no MKV)", dim);
        const int ew = laySbs ? ytW / 2 : ytW, eh = laySbs ? ytH : ytH / 2;
        swprintf_s(buf, L"each eye %dx%d, shown %s", ew, eh, big.ch < eyeH ? cropShown : L"full");
        text(8, 124, w - 4, buf, dim);
    } else if (!wantMkv && !want8k) {
        text(ox, oy + 2, w - 4, L"No 3D copy is made", txt);
        text(ox, oy + 18, w - 4, L"(only the original .mp4)", dim);
    } else {
        drawFrame(plain ? recW : shown.fw, plain ? recH : shown.fh, plain || laySbs, plain || shown.sw <= 0 ? 0.0 : (double)shown.vx / shown.sw);
        text(ox, 3, w - 4, want8k ? L"YouTube 8K" : L"3D .mkv", dim);

        int ly = 94;   // text lines under the pictures
        if (wantMkv) {
            if (remux) swprintf_s(buf, L"3D .mkv: %dx%d (copied as is)", recW, recH);
            else swprintf_s(buf, L"3D .mkv: %dx%d", nat.fw, nat.fh);
            text(8, ly, w - 4, buf, txt);
            ly += 15;
        }
        if (want8k) {
            swprintf_s(buf, L"YouTube 8K: %dx%d", big.fw, big.fh);
            text(8, ly, w - 4, buf, txt);
            ly += 15;
        }
        if (!plain) {
            swprintf_s(buf, L"each eye %dx%d, shown %s", shown.sw, shown.sh,
                       shown.ch < eyeH ? cropShown : L"full");
            text(8, ly, w - 4, buf, dim);
        }
    }
    if (oldFont) SelectObject(dc, oldFont);
    ModifyWorldTransform(dc, nullptr, MWT_IDENTITY);
    BitBlt(di->hDC, rc.left, rc.top, pw, ph, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void SetControls(const KeyValues& kv) {
    for (const auto& e : kv) {
        const Field* f = FindField(e.first);
        if (!f) continue;
        HWND h = Ctl(f->id);
        switch (f->kind) {
        case Kind::Edit: SetWindowTextW(h, e.second.c_str()); break;
        case Kind::Check: SendMessageW(h, BM_SETCHECK, e.second == L"1" ? BST_CHECKED : BST_UNCHECKED, 0); break;
        case Kind::Combo: {
            const std::wstring opts = f->options, cur = Norm(e.first, e.second);
            int idx = 0, sel = 0;
            bool found = false;
            size_t pos = 0;
            while (pos <= opts.size()) {
                size_t bar = opts.find(L'|', pos);
                if (bar == std::wstring::npos) bar = opts.size();
                if (opts.substr(pos, bar - pos) == cur) { sel = idx; found = true; }
                ++idx;
                pos = bar + 1;
            }
            if (f->custom) {   // idx = number of options; an extra item past them stands for a value set by hand
                if ((int)SendMessageW(h, CB_GETCOUNT, 0, 0) > idx) SendMessageW(h, CB_DELETESTRING, (WPARAM)idx, 0);
                for (auto it = g_customValue.begin(); it != g_customValue.end(); ++it)
                    if (it->first == f->id) { g_customValue.erase(it); break; }
                if (!found && !cur.empty()) {
                    wchar_t item[96];
                    swprintf_s(item, f->custom, cur.c_str());
                    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)item);
                    g_customValue.emplace_back(f->id, cur);
                    sel = idx;
                }
            }
            SendMessageW(h, CB_SETCURSEL, (WPARAM)sel, 0);
            break;
        }
        case Kind::Track: {
            const double v = wcstod(e.second.c_str(), nullptr);
            int pos = (int)(v * 20.0 + 0.5);
            pos = pos < 0 ? 0 : (pos > 20 ? 20 : pos);
            SendMessageW(h, TBM_SETPOS, TRUE, pos);
            g_smoothPos = pos;
            g_smoothText = e.second;
            wchar_t b[16];
            swprintf_s(b, L"%.2f", pos / 20.0);
            SetWindowTextW(Ctl(ID_SMOOTHVAL), b);
            break;
        }
        }
    }
    YtModeFromChecks();
    UpdateLayoutEnabled();
    UpdateBitrateHint();
    UpdateRateControlEnabled();
    UpdateYt8kEnabled();
    UpdateFreeSpace();
    UpdatePreview();
    UpdateLayoutPreview();
}

KeyValues CollectControls() {
    KeyValues kv;
    ChecksFromYtMode();
    for (const Field& f : kFields) {
        HWND h = Ctl(f.id);
        std::wstring v;
        switch (f.kind) {
        case Kind::Edit: v = Trim(GetText(f.id)); break;
        case Kind::Check: v = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0"; break;
        case Kind::Combo: {
            const int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
            const std::wstring opts = f.options;
            int idx = 0;
            size_t pos = 0;
            while (pos <= opts.size()) {
                size_t bar = opts.find(L'|', pos);
                if (bar == std::wstring::npos) bar = opts.size();
                if (idx == sel) { v = opts.substr(pos, bar - pos); break; }
                ++idx;
                pos = bar + 1;
            }
            if (v.empty() && sel >= 0 && sel == idx && f.custom)   // the "Custom (...)" item: keep the file value
                for (const auto& cv : g_customValue) if (cv.first == f.id) v = cv.second;
            break;
        }
        case Kind::Track: {
            const int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
            v = pos == g_smoothPos ? g_smoothText : FloatText(pos * 0.05);
            break;
        }
        }
        kv.emplace_back(f.key, v);
    }
    return kv;
}

void LoadFromFile() {
    Config c;
    std::vector<std::wstring> problems;
    ParseConfigFile(g_path, c, problems);
    if (c.hotkey.text.empty()) ParseHotkey(kDefaultHotkey, c.hotkey);
    SetControls(FromConfig(c));
    g_loadedSvrMode = elog::Widen(SteamVrModeKey(c.steamvr.mode));
    // a config from before 0.5.0 has no start_with_windows: the check box shows the Run value instead (Save then adds the key)
    if (!c.startWithWindowsSet)
        SendMessageW(Ctl(ID_AUTOSTART), BM_SETCHECK, AutostartChecked() ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!problems.empty()) {
        std::wstring m = L"The configuration file contains invalid values on the following lines. The window shows the default value for each of them, and Save replaces them:\n\n";
        for (const auto& p : problems) m += p + L"\n";
        Report(kWindowTitle, m);
    }
}

// ---- "Conversions" group: the running job (status text, progress bar, Pause / Cancel) and the waiting list (Start now /
// Remove). Polled from the worker through g_hooks on kQueueTimer and on WM_QUEUE_POKE; controls are only touched when their
// content changes (no flicker while the window refreshes twice a second).
void SetTextIfChanged(HWND h, const std::wstring& t) {
    if (!h) return;
    wchar_t cur[512] = L"";
    GetWindowTextW(h, cur, 512);
    if (t != cur) SetWindowTextW(h, t.c_str());
}

// `t` cut to `w` pixels in the dialog font with a trailing "..." (single line; the whole text when it fits).
std::wstring FitText(HWND ref, const std::wstring& t, int w) {
    HDC dc = GetDC(ref);
    HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
    auto width = [&](const std::wstring& x) { SIZE z{}; GetTextExtentPoint32W(dc, x.c_str(), (int)x.size(), &z); return (int)z.cx; };
    std::wstring r = t;
    if (width(r) > w) {
        size_t n = r.size();
        while (n > 0 && width(r.substr(0, n) + L"...") > w) --n;
        r = r.substr(0, n) + L"...";
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(ref, dc);
    return r;
}

// The selected waiting-list items (multi-select list box), as job ids, in list order.
std::vector<unsigned long long> SelectedJobs() {
    std::vector<unsigned long long> ids;
    HWND lb = Ctl(ID_QLIST);
    const int n = (int)SendMessageW(lb, LB_GETSELCOUNT, 0, 0);
    if (n <= 0) return ids;
    std::vector<int> idx((size_t)n);
    const int got = (int)SendMessageW(lb, LB_GETSELITEMS, (WPARAM)n, (LPARAM)idx.data());
    for (int k = 0; k < got; ++k)
        if (idx[(size_t)k] >= 0 && idx[(size_t)k] < (int)g_qIds.size()) ids.push_back(g_qIds[(size_t)idx[(size_t)k]]);
    return ids;
}

void UpdateQueueButtons() {
    const std::vector<unsigned long long> sel = SelectedJobs();
    bool startable = false, anyWaiting = false;   // a selected job / any job that is not already forced ("next")
    if (g_hooks.queued)
        for (const auto& q : g_hooks.queued()) {
            if (q.force) continue;
            anyWaiting = true;
            for (unsigned long long id : sel) if (q.id == id) startable = true;
        }
    EnableWindow(Ctl(ID_QSTART), startable && g_hooks.start);
    EnableWindow(Ctl(ID_QSTARTALL), anyWaiting && g_hooks.startAll);
    EnableWindow(Ctl(ID_QREMOVE), !sel.empty() && g_hooks.remove);
}

// The format snapshot of the values the window shows now (saved or not): YouTube output, 2D video, keep MP4, output layout,
// eye crop, 8K codec / bitrate. Same mapping as Save (CollectControls).
pp::JobFormat WindowFormat() {
    pp::JobFormat f;
    for (const auto& e : CollectControls()) {
        const std::wstring& k = e.first;
        const std::wstring v = Lower(Trim(e.second));
        if (k == L"post_mkv") f.postMkv = v == L"1";
        else if (k == L"post_flat") f.postFlat = v == L"1";
        else if (k == L"post_yt8k") f.postYt8k = v == L"1";
        else if (k == L"keep_mp4") f.keepMp4 = v == L"1";
        else if (k == L"layout_3d")
            f.layout3d = v == L"sbs_half" ? Layout3d::SbsHalf : v == L"sbs_full" ? Layout3d::SbsFull : v == L"tb_full" ? Layout3d::TbFull : Layout3d::TbHalf;
        else if (k == L"eye_crop") f.eyeCrop = pp::CropFromName(elog::Narrow(v));
        else if (k == L"yt8k_codec") f.yt8kCodec = v == L"av1" ? VideoCodec::Av1 : VideoCodec::Hevc;
        else if (k == L"yt8k_bitrate_mbps") {
            const unsigned long b = wcstoul(v.c_str(), nullptr, 10);
            f.yt8kBitrateMbps = b >= 20 && b <= 400 ? (unsigned)b : 150;
        }
    }
    return f;
}

// The Advanced "Tools:" row: greyed without its hook; the register button reads Register / Unregister (hidden when the
// tray does not handle registration, e.g. test mode). Called with the queue view (500 ms timer).
void UpdateToolButtons() {
    EnableWindow(Ctl(ID_OPENLOG), (bool)g_tools.openLog);
    EnableWindow(Ctl(ID_TAG3D), (bool)g_tools.tag3d);
    const int rs = g_tools.regState ? g_tools.regState() : 0;
    if (HWND rb = Ctl(ID_REGLAYER)) {
        SetShown(rb, rs != 0);
        SetTextIfChanged(rb, rs == 2 ? L"Unregister from VR games..." : L"Register for VR games...");
    }
}

void UpdateQueueView() {
    if (!g_dlg) return;
    UpdateToolButtons();
    EnableWindow(Ctl(ID_CONVERTPICK), (bool)g_hooks.pickFiles);
    EnableWindow(Ctl(ID_ADDQUEUE), (bool)g_hooks.pickFiles);
    if (!g_hooks.state || !g_hooks.queued) {
        SetTextIfChanged(Ctl(ID_QSTATUS), L"Conversion status is not available.");
        for (int id : {ID_QPAUSE, ID_QCANCEL, ID_QSTART, ID_QSTARTALL, ID_QREMOVE}) EnableWindow(Ctl(id), FALSE);
        return;
    }
    const pp::State s = g_hooks.state();
    const std::vector<pp::QueuedJob> q = g_hooks.queued();
    size_t held = 0;
    for (const auto& j : q) if (!j.force && !j.userHeld && s.held) ++held;

    // status of the running job: three lines, each cut to the width with "..." (a long file name never wraps or clips)
    RECT sr{};
    GetClientRect(Ctl(ID_QSTATUS), &sr);
    const int sw = sr.right - sr.left - 2;
    auto fit = [&](const std::wstring& t) { return FitText(Ctl(ID_QSTATUS), t, sw); };
    std::wstring st;
    if (s.busy) {
        std::wstring l3 = s.step + L" step";
        if (s.percent >= 0) l3 += L"  " + std::to_wstring(s.percent) + L"%";
        if (s.cancelling) l3 += L" - cancelling...";
        else if (s.userPaused) l3 += L" - paused";
        else if (s.paused) l3 += L" - waiting: converts " + s.holdReason;
        else if (s.etaSec >= 0) l3 += L" - " + pp::EtaText(s.etaSec);
        st = fit(s.file) + L"\r\n" + fit(L"Format: " + s.format) + L"\r\n" + fit(l3);
    } else {
        st = L"Nothing is converting.";
        std::wstring l2;
        if (held) l2 = std::to_wstring(held) + (held == 1 ? L" recording waiting" : L" recordings waiting") + L" -- converts " + s.holdReason + L".";
        if (s.userHeld) l2 += (l2.empty() ? L"" : L" ") + std::to_wstring(s.userHeld) + L" waiting for you (Start).";
        if (!l2.empty()) st += L"\r\n" + fit(l2);
        if (!s.lastLine.empty()) st += L"\r\n" + fit(std::wstring(1, (wchar_t)towupper(s.lastLine[0])) + s.lastLine.substr(1));
    }
    SetTextIfChanged(Ctl(ID_QSTATUS), st);

    HWND pb = Ctl(ID_QPROGRESS);
    const int pos = s.busy && s.percent >= 0 ? s.percent : 0;
    if ((int)SendMessageW(pb, PBM_GETPOS, 0, 0) != pos) SendMessageW(pb, PBM_SETPOS, (WPARAM)pos, 0);
    const int pbState = s.busy && s.paused ? PBST_PAUSED : PBST_NORMAL;
    if ((int)SendMessageW(pb, PBM_GETSTATE, 0, 0) != pbState) SendMessageW(pb, PBM_SETSTATE, (WPARAM)pbState, 0);
    if ((IsWindowEnabled(pb) != FALSE) != s.busy) EnableWindow(pb, s.busy);   // a faint empty track while nothing runs

    // Pause / Resume / Run now (a running job held by post_when, not paused by the user: Run now forces it)
    g_qRunNow = s.busy && s.paused && !s.userPaused && !s.forced;
    SetTextIfChanged(Ctl(ID_QPAUSE), g_qRunNow ? L"Run now" : (s.busy && s.userPaused ? L"Resume" : L"Pause"));
    const bool canAct = s.busy && !s.cancelling;
    EnableWindow(Ctl(ID_QPAUSE), canAct && (g_qRunNow ? (bool)g_hooks.start : (bool)g_hooks.togglePause));
    EnableWindow(Ctl(ID_QCANCEL), canAct && g_hooks.cancel);

    // waiting list: "<file>  -  <format>  -  <state>", in the order they will run (forced first, then the rest, oldest first)
    std::vector<std::wstring> items;
    std::vector<unsigned long long> ids;
    std::vector<const pp::QueuedJob*> order;
    for (const auto& j : q) if (j.force) order.push_back(&j);
    for (size_t a = 1; a < order.size(); ++a)   // forced: in the order they were forced (= the order they run)
        for (size_t b2 = a; b2 > 0 && order[b2]->forceSeq < order[b2 - 1]->forceSeq; --b2) std::swap(order[b2], order[b2 - 1]);
    for (const auto& j : q) if (!j.force) order.push_back(&j);
    bool nextShown = false;
    for (const pp::QueuedJob* jp : order) {
        const pp::QueuedJob& j = *jp;
        const size_t slash = j.path.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? j.path : j.path.substr(slash + 1);
        std::wstring t = name + L"  -  " + pp::FormatText(j.fmt) + (j.ownFormat ? L"" : L" (current settings)") + L"  -  ";
        const bool startsInTurn = j.force || (!s.held && !j.userHeld);
        if (startsInTurn && !nextShown) { t += L"next"; nextShown = true; }
        else if (j.force) t += L"queued (started by you)";
        else if (j.userHeld) t += L"waiting for you";
        else if (s.held) t += L"waiting: converts " + s.holdReason;
        else t += L"queued";
        items.push_back(t);
        ids.push_back(j.id);
    }
    if (items.empty()) items.push_back(L"(no recordings waiting)");
    if (items != g_qItems) {
        HWND lb = Ctl(ID_QLIST);
        const std::vector<unsigned long long> keep = SelectedJobs();
        SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
        SendMessageW(lb, LB_RESETCONTENT, 0, 0);
        int widest = 0;
        HDC dc = GetDC(lb);
        HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
        for (const auto& t : items) {
            SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t.c_str());
            SIZE z{};
            GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &z);
            if (z.cx > widest) widest = z.cx;
        }
        if (old) SelectObject(dc, old);
        ReleaseDC(lb, dc);
        SendMessageW(lb, LB_SETHORIZONTALEXTENT, (WPARAM)(widest + 8), 0);   // long rows scroll sideways instead of being cut
        for (size_t i = 0; i < ids.size(); ++i)
            for (unsigned long long id : keep) if (ids[i] == id) SendMessageW(lb, LB_SETSEL, TRUE, (LPARAM)i);
        SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lb, nullptr, TRUE);
        EnableWindow(lb, !ids.empty());
        g_qItems = items;
        g_qIds = ids;
    }
    UpdateQueueButtons();
}

void QueueCommand(int id, int code) {
    if (id == ID_QLIST) {
        if (code == LBN_SELCHANGE) UpdateQueueButtons();
        return;
    }
    if (code != BN_CLICKED) return;
    switch (id) {
    case ID_CONVERTPICK:
    case ID_ADDQUEUE:
        if (g_hooks.pickFiles) {
            const pp::JobFormat f = WindowFormat();
            if (f.Empty()) {
                Report(kWindowTitle, L"The Export settings create no files (YouTube output None and no 2D video). Choose a YouTube output first.");
                break;
            }
            g_hooks.pickFiles(g_dlg, f, id == ID_CONVERTPICK);
        }
        break;
    case ID_QPAUSE:
        if (g_qRunNow) {
            if (g_hooks.start && g_hooks.state) {
                const pp::State s = g_hooks.state();
                if (s.busy && s.runningId) g_hooks.start(s.runningId);
            }
        } else if (g_hooks.togglePause) {
            g_hooks.togglePause();
        }
        break;
    case ID_QCANCEL:
        if (g_hooks.cancel) g_hooks.cancel();
        break;
    case ID_QSTART:   // every selected job, in list order (= the order they run in)
        if (g_hooks.start) for (unsigned long long j : SelectedJobs()) g_hooks.start(j);
        break;
    case ID_QSTARTALL:
        if (g_hooks.startAll) g_hooks.startAll();
        break;
    case ID_QREMOVE:
        if (g_hooks.remove) for (unsigned long long j : SelectedJobs()) g_hooks.remove(j);
        break;
    default: break;
    }
    UpdateQueueView();
}

const UINT_PTR kCaptureTimer = 2;   // 30 ms while "Press a key" is armed: polls input::TakeCaptured (pads / wheels)

// mode 0 = off, 1 = replace the field, 2 = add to the list. Pad / joystick capture (tray/input.h) follows the armed state.
void ArmCapture(int mode) {
    g_capturing = mode;
    SetWindowTextW(Ctl(ID_HOTKEY_PICK), mode == 1 ? L"Press a key... (Esc cancels)" : L"Capture...");
    SetWindowTextW(Ctl(ID_HOTKEY_ADD), mode == 2 ? L"Press to add... (Esc cancels)" : L"Add...");
    input::SetCapture(mode != 0);
    if (mode) SetTimer(g_dlg, kCaptureTimer, 30, nullptr);
    else KillTimer(g_dlg, kCaptureTimer);
}

// A captured binding text: replaces the field (mode 1) or is appended to the list (mode 2; replaces an empty / "none"
// field; nothing happens when the list already has it). Disarms.
void ApplyCaptured(const std::wstring& t) {
    std::wstring v = t;
    if (g_capturing == 2) {
        const std::wstring cur = Trim(GetText(ID_HOTKEY));
        Hotkey h;
        if (!cur.empty() && Lower(cur) != L"none") {
            bool have = false;
            if (ParseHotkey(cur, h)) for (const auto& b : h.bindings) if (b.text == t) have = true;
            v = have ? cur : cur + L", " + t;
        }
    }
    SetWindowTextW(Ctl(ID_HOTKEY), v.c_str());
    ArmCapture(0);
}

// The saved "Theme" choice: glass follows Windows (system) or is forced light / dark; the window repaints at once.
void ApplyThemeChoice() {
    const int sel = (int)SendMessageW(Ctl(ID_THEME), CB_GETCURSEL, 0, 0);   // 0 system, 1 light, 2 dark (the options' order)
    glass::ForceDark(sel == 1 ? 0 : sel == 2 ? 1 : -1);
    glass::Repaint(g_dlg);
    ApplyGlassTheme();
}

// The window's values without steamvr_mode while it is still the loaded one: the SteamVR games window may change that key in
// the file while this window is open (DoSave and the unsaved-changes check both use this).
KeyValues ComparableValues() {
    KeyValues kv = CollectControls();
    for (auto it = kv.begin(); it != kv.end(); ++it)
        if (it->first == L"steamvr_mode" && it->second == g_loadedSvrMode) { kv.erase(it); break; }
    return kv;
}

// Save: writes config.ini; the window stays open (Exit closes it). false = nothing was saved (bad values, reported).
bool DoSave() {
    SaveResult r;
    // steamvr_mode is also set by the SteamVR games window, possibly while this window is open: written only when its drop-down
    // was changed here (else a mode picked there meanwhile would be put back)
    const KeyValues kv = ComparableValues();
    if (!SaveKeyValuesToFile(g_path, kv, r)) {
        std::wstring m = L"The settings could not be saved:\n\n";
        for (const auto& p : r.problems) m += p + L"\n";
        Report(L"Not Saved", m);
        return false;
    }
    for (const auto& p : kv) if (p.first == L"steamvr_mode") g_loadedSvrMode = p.second;   // saved: now the file's value
    g_savedKv = ComparableValues();
    // "Start with Windows": the choice is the config key start_with_windows (written above); the Run value follows it here
    // (a user action -- the only other writer is the tray's start-up repair while the key is 1)
    const bool wantAuto = SendMessageW(Ctl(ID_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool hadAuto = AutostartChecked();
    if (wantAuto != hadAuto) {
        std::wstring err;
        if (install::SetAutostart(wantAuto, ExeFolder(), err)) {
            LOGI("settings: start with Windows %s (HKCU Run value %s)", wantAuto ? "on" : "off", wantAuto ? "written" : "deleted");
            // Windows' own switch (Task Manager > Startup apps) is never written by this program: only pointed out
            if (wantAuto && install::ReadAutostart().disabledInTaskManager)
                Report(kWindowTitle, L"Windows has this disabled in Task Manager > Startup apps; turn it on there.");
        } else
            Report(kWindowTitle, L"The Start with Windows setting could not be changed:\n" + err);
    }
    ApplyThemeChoice();
    if (g_onSaved) g_onSaved(r.changed);
    SetWindowTextW(Ctl(ID_SAVE), L"Saved");
    SetTimer(g_dlg, kSavedTimer, 1500, nullptr);
    return true;
}

// Exit / Alt+F4 / the taskbar's Close: asks Save / Don't save / Cancel when the window holds unsaved changes (never in test mode,
// where Exit closes without saving like the old Cancel button).
void TryClose() {
    if (!g_reporter && ComparableValues() != g_savedKv) {
        const int a = glass::MessageBox(g_dlg, L"Save the changes before closing?", kWindowTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL) return;
        if (a == IDYES && !DoSave()) return;
    }
    DestroyWindow(g_dlg);
}

// "Browse...": the modern folder picker (IFileOpenDialog + FOS_PICKFOLDERS; any drive), opened at the current folder (or the
// default captures folder). The free-space line is updated at once.
void DoBrowse() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        Report(kWindowTitle, L"The folder picker could not be opened. Type the folder into the field instead.");
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Select the folder for recordings (and the converted files)");
    std::wstring start = Trim(GetText(ID_OUTDIR));
    if (start.empty()) start = DefaultCapturesDir(ExeFolder());
    while (!start.empty() && GetFileAttributesW(start.c_str()) == INVALID_FILE_ATTRIBUTES) {   // nearest existing parent
        const size_t slash = start.find_last_of(L"\\/");
        if (slash == std::wstring::npos) { start.clear(); break; }
        start.resize(slash);
    }
    IShellItem* folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
        dlg->SetFolder(folder);
        folder->Release();
    }
    if (SUCCEEDED(dlg->Show(g_dlg))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                SetWindowTextW(Ctl(ID_OUTDIR), p);
                CoTaskMemFree(p);
                UpdateFreeSpace();   // at once (typing waits for kFreeSpaceTimer)
            }
            item->Release();
        }
    }
    dlg->Release();
}

// The window is shorter than its content: the content scrolls, the footer stays put. Relayout places every content control
// at its layout position minus g_scrollY.
void ScrollContent(int pos) {
    if (g_scrollMax <= 0 || !g_dlg) return;
    pos = pos < 0 ? 0 : (pos > g_scrollMax ? g_scrollMax : pos);
    if (pos == g_scrollY) return;
    g_scrollY = pos;
    Relayout();
    SetScrollPos(g_dlg, SB_VERT, pos, TRUE);
}
// Keyboard focus moved to `c`: show its tab and scroll it into view.
void EnsureVisible(HWND c) {
    if (!c || !g_dlg) return;
    if (const int t = TabOf(c); t >= 0) SetTab(t);
    if (g_scrollMax <= 0) return;
    RECT r;
    GetWindowRect(c, &r);
    MapWindowPoints(nullptr, g_dlg, (POINT*)&r, 2);
    const int top = (int)std::floor(r.top / g_s), bottom = (int)std::ceil(r.bottom / g_s);   // layout units
    if (top < 0) ScrollContent(g_scrollY + top - 8);
    else if (bottom > g_viewH) ScrollContent(g_scrollY + bottom - g_viewH + 8);
}

void CommandHandler(int id, int code) {
    if (id == ID_QLIST && code == LBN_SETFOCUS) { ShowHelp(HelpText(id)); g_hover = Ctl(id); EnsureVisible(Ctl(id)); return; }
    if (((id >= ID_CONVERTPICK && id <= ID_QREMOVE) || id == ID_ADDQUEUE || id == ID_QSTARTALL) && code != BN_SETFOCUS) {
        QueueCommand(id, code);
        return;
    }
    // keyboard focus (tab / click) shows the help of that control; buttons only send BN_SETFOCUS with BS_NOTIFY
    if (code == EN_SETFOCUS || code == CBN_SETFOCUS || code == BN_SETFOCUS) {
        if (id != ID_HELPTEXT) { ShowHelp(HelpText(id)); g_hover = Ctl(id); EnsureVisible(Ctl(id)); }
        return;
    }
    if ((id == ID_INDX || id == ID_INDY || id == ID_INDZ || id == ID_INDSIZE || id == ID_INDOPACITY) && code == EN_CHANGE) UpdatePreview();
    if (id == ID_INDICATOR && code == BN_CLICKED) UpdatePreview();
    if (id == ID_RECORDYT && code == BN_CLICKED) { UpdateYt8kEnabled(); UpdateLayoutPreview(); }
    if (id == ID_PROJECTION && code == CBN_SELCHANGE) UpdateLayoutEnabled();
    if ((id == ID_BITRATE || id == ID_CODEC) && code == CBN_SELCHANGE) UpdateBitrateHint();
    if (id == ID_RATECONTROL && code == CBN_SELCHANGE) UpdateRateControlEnabled();
    if ((id == ID_YTMODE || id == ID_PROJECTION) && code == CBN_SELCHANGE) UpdateYt8kEnabled();
    if (id == ID_OUTDIR && code == EN_CHANGE) SetTimer(g_dlg, kFreeSpaceTimer, kFreeSpaceDelayMs, nullptr);   // debounced: not per key
    if ((id == ID_PROJECTION || id == ID_YTMODE || id == ID_LAYOUT3D || id == ID_EYECROP || id == ID_MAXWIDTH || id == ID_CODEC) &&
        code == CBN_SELCHANGE)
        UpdateLayoutPreview();
    if ((id == ID_PROJECTION || id == ID_YTMODE || id == ID_LAYOUT3D || id == ID_POSTWHEN || id == ID_EYECROP ||
         id == ID_MAXWIDTH || id == ID_BITRATE || id == ID_YT8KBITRATE || id == ID_YT8KCODEC || id == ID_RATECONTROL ||
         id == ID_PRESET || id == ID_BITDEPTH || id == ID_SHARPEN) && code == CBN_SELCHANGE) {
        ShowHelp(HelpText(id));
        return;
    }
    if ((id == ID_SAVE || id == ID_CANCEL || id == ID_MINIMIZE || id == ID_OPENINI || id == ID_DEFAULTS || id == ID_BROWSE ||
         id == ID_OPENSTEAMVR) && code != BN_CLICKED) return;
    switch (id) {
    case ID_SAVE: DoSave(); break;
    case ID_CANCEL: TryClose(); break;
    case ID_MINIMIZE: ShowWindow(g_dlg, SW_MINIMIZE); break;
    case ID_OPENSTEAMVR:
        if (g_openSteamVr) {   // that window lays out in 96-DPI pixels: it is made DPI-unaware like the rest of the tray
            const DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
            g_openSteamVr();
            if (old) SetThreadDpiAwarenessContext(old);
        }
        break;
    case ID_UPDATENOW: if (code == BN_CLICKED && g_update.checkNow) g_update.checkNow(); break;
    case ID_OPENLOG: if (code == BN_CLICKED && g_tools.openLog) g_tools.openLog(); break;
    case ID_REGLAYER: if (code == BN_CLICKED && g_tools.toggleReg) g_tools.toggleReg(); break;
    case ID_TAG3D:
        if (code == BN_CLICKED && g_tools.tag3d) {   // a small menu under the button: which 3D layout the video has
            RECT br{};
            GetWindowRect(Ctl(ID_TAG3D), &br);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Side by side...");
            AppendMenuW(m, MF_STRING, 2, L"Top / bottom...");
            glass::ThemeMenus();
            const UINT pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom, 0, g_dlg, nullptr);
            DestroyMenu(m);
            if (pick) g_tools.tag3d(g_dlg, pick == 2);
        }
        break;
    case ID_OPENINI: ShellExecuteW(nullptr, L"open", g_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_DEFAULTS: {
        Config d;
        ParseHotkey(kDefaultHotkey, d.hotkey);
        SetControls(FromConfig(d));
        SendMessageW(Ctl(ID_AUTOSTART), BM_SETCHECK, BST_UNCHECKED, 0);   // default: off
        break;
    }
    case ID_BROWSE: DoBrowse(); break;
    case ID_HOTKEY_PICK:
    case ID_HOTKEY_ADD:
        if (code == BN_CLICKED) {
            const int mode = id == ID_HOTKEY_PICK ? 1 : 2;
            ArmCapture(g_capturing == mode ? 0 : mode);   // clicking the armed button again cancels
            if (g_capturing) SetFocus(Ctl(id));
        }
        break;
    default: break;
    }
}

// Called from the message loop while "Press a key" is armed: key messages, and the middle / side mouse buttons (never
// left / right: they drive the window). True = message consumed. Pad / wheel buttons arrive through kCaptureTimer.
bool CaptureKey(const MSG& m) {
    if (!g_capturing) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0,
               alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    unsigned vk = 0;
    if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
        vk = (unsigned)m.wParam;
        if (vk == VK_SNAPSHOT) return true;   // PrintScreen: Windows sends only its key-up (handled below)
    } else if (m.message == WM_KEYUP || m.message == WM_SYSKEYUP) {
        if ((unsigned)m.wParam != VK_SNAPSHOT) return false;
        vk = VK_SNAPSHOT;
    } else if (m.message == WM_MBUTTONDOWN || m.message == WM_NCMBUTTONDOWN) {
        vk = VK_MBUTTON;
    } else if (m.message == WM_XBUTTONDOWN || m.message == WM_NCXBUTTONDOWN) {
        vk = HIWORD(m.wParam) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
    } else if (m.message == WM_MBUTTONUP || m.message == WM_XBUTTONUP || m.message == WM_NCMBUTTONUP ||
               m.message == WM_NCXBUTTONUP) {
        return true;   // the release of a captured button (no "Back" app command in the window)
    } else {
        return false;
    }
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
        vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU)
        return true;   // wait for the main key
    if (vk == VK_ESCAPE && !ctrl && !shift && !alt) { ArmCapture(0); return true; }
    std::wstring t;
    if (!HotkeyText(vk, ctrl, shift, alt, t)) {
        MessageBeep(MB_ICONEXCLAMATION);
        return true;   // keep waiting for a key the config can name (only Escape with modifiers gets here)
    }
    ApplyCaptured(t);
    return true;
}

const UINT_PTR kHoverTimer = 1;

// The glass control class for a classic one (tray/glass.h); EDIT and LISTBOX stay native (glass::FrameEdit).
const wchar_t* GlassClass(const wchar_t* cls) {
    if (!_wcsicmp(cls, L"BUTTON")) return glass::kButton;
    if (!_wcsicmp(cls, L"STATIC")) return glass::kStatic;
    if (!_wcsicmp(cls, L"COMBOBOX")) return glass::kCombo;
    if (!_wcsicmp(cls, TRACKBAR_CLASSW)) return glass::kSlider;
    if (!_wcsicmp(cls, PROGRESS_CLASSW)) return glass::kProgress;
    if (!_wcsicmp(cls, WC_LINK)) return glass::kLink;
    return cls;
}

// Colours that are not the theme's defaults: grey hints, the red frame-rate warning, the footer strip, the logo. After the
// Create pass and whenever Windows switches light / dark / accent.
void ApplyGlassTheme() {
    for (HWND d : g_dim) glass::SetDim(d, true);
    if (HWND w = Ctl(ID_FPSWARN)) glass::SetTextColor(w, glass::Current().dark ? RGB(255, 153, 164) : RGB(196, 43, 28));
    if (HWND f = Ctl(kFooterBgId)) glass::SetPanel(f);
    for (int id : {ID_LOGO, ID_INDPREVIEW, ID_LAYOUTPREVIEW, ID_TABS})
        if (HWND l = Ctl(id)) glass::SetOwnerDrawTransparent(l);
    if (g_tip) SetWindowTheme(g_tip, glass::Current().dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (g_dlg) SetWindowTheme(g_dlg, glass::Current().dark ? L"DarkMode_Explorer" : nullptr, nullptr);   // the window's scroll bar
    if (g_dlg) InvalidateRect(g_dlg, nullptr, TRUE);
}

LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        CommandHandler(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == Ctl(ID_SMOOTH)) {
            wchar_t b[16];
            swprintf_s(b, L"%.2f", (int)SendMessageW(Ctl(ID_SMOOTH), TBM_GETPOS, 0, 0) / 20.0);
            SetWindowTextW(Ctl(ID_SMOOTHVAL), b);
        }
        return 0;
    case WM_VSCROLL:
        if (!lp) {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int pos = g_scrollY;
            switch (LOWORD(wp)) {
            case SB_LINEUP: pos -= 30; break;
            case SB_LINEDOWN: pos += 30; break;
            case SB_PAGEUP: pos -= (int)si.nPage; break;
            case SB_PAGEDOWN: pos += (int)si.nPage; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = g_scrollMax; break;
            default: break;
            }
            ScrollContent(pos);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // over the dot preview: the wheel moves the dot nearer / farther instead of scrolling the window
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        RECT pr{};
        if (HWND pv = Ctl(ID_INDPREVIEW); pv && IsWindowVisible(pv) && GetWindowRect(pv, &pr) && PtInRect(&pr, pt)) {
            PreviewWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        if (g_scrollMax > 0) { ScrollContent(g_scrollY - (int)GET_WHEEL_DELTA_WPARAM(wp) * 30 / WHEEL_DELTA); return 0; }
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_NOTIFY: {
        const NMHDR* nh = (const NMHDR*)lp;
        if (nh && nh->idFrom == ID_UPDATELINK && (nh->code == NM_CLICK || nh->code == NM_RETURN)) {
            if (g_update.startUpdate) g_update.startUpdate();   // confirm + download + run (same as the tray menu item)
            return 0;
        }
        if (nh && nh->idFrom == ID_ABOUTLINK && (nh->code == NM_CLICK || nh->code == NM_RETURN)) {
            const NMLINK* nl = (const NMLINK*)lp;
            ShellExecuteW(nullptr, L"open", nl->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_DRAWITEM:
        if (wp == ID_INDPREVIEW) { DrawPreview((const DRAWITEMSTRUCT*)lp); return TRUE; }
        if (wp == ID_LAYOUTPREVIEW) { DrawLayoutPreview((const DRAWITEMSTRUCT*)lp); return TRUE; }
        if (wp == ID_LOGO) { DrawLogo((const DRAWITEMSTRUCT*)lp); return TRUE; }
        if (wp == ID_TABS) { DrawTabs((const DRAWITEMSTRUCT*)lp); return TRUE; }
        return DefWindowProcW(h, msg, wp, lp);
    case kMsgShowTabOf: {
        const int t = TabOf(Ctl((int)wp));
        if (t >= 0) SetTab(t);
        if (HWND s = Ctl(ID_TABS)) InvalidateRect(s, nullptr, FALSE);
        return t;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g_capturing) ArmCapture(0);
        return 0;
    case WM_SETCURSOR:
        // standard controls forward this to the parent: wp = the child under the mouse (or the dialog itself)
        Hover((HWND)wp);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_TIMER:
        if (wp == kCaptureTimer) {   // a pad / wheel chord was pressed and released (tray/input.h)
            std::wstring t;
            if (g_capturing && input::TakeCaptured(t)) ApplyCaptured(t);
            return 0;
        }
        if (wp == kPulseTimer) {
            if (g_prev.on && !IsIconic(h)) InvalidateRect(Ctl(ID_INDPREVIEW), nullptr, FALSE);
            return 0;
        }
        if (wp == kQueueTimer) {
            if (!IsIconic(h)) { UpdateQueueView(); UpdateFpsWarning(); UpdateUpdateLine(); }
            return 0;
        }
        if (wp == kFreeSpaceTimer) {   // one-shot: UpdateFreeSpace kills it
            UpdateFreeSpace();
            return 0;
        }
        if (wp == kSavedTimer) {   // one-shot: "Saved" back to "Save"
            KillTimer(h, kSavedTimer);
            SetWindowTextW(Ctl(ID_SAVE), L"Save");
            return 0;
        }
        // some controls (edit, trackbar) may keep WM_SETCURSOR to themselves: also look at what is under the mouse
        if (wp == kHoverTimer) {
            POINT pt;
            if (GetCursorPos(&pt)) {
                HWND w = WindowFromPoint(pt);
                if (w == h || (w && IsChild(h, w))) Hover(w);
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:   // read-only edits (the Description panel)
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)glass::EditBrush((HDC)wp, (HWND)lp);
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        glass::PaintBackground(h, (HDC)wp, rc);
        return 1;
    }
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
        if (glass::OnSettingChange(h)) ApplyGlassTheme();
        return 0;
    case WM_QUEUE_POKE:
        UpdateQueueView();
        UpdateFpsWarning();
        return 0;
    case WM_UPDATE_POKE:
        UpdateUpdateLine();
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Relayout();
        return 0;
    case WM_DPICHANGED: {
        // moved to a screen with another scale: new fonts for every control, the drop-down lists widened to match, the size
        // Windows suggests (the layout units stay, so the content scales with the window), the help panel measured again
        const double old = g_s;
        g_s = HIWORD(wp) / 96.0;
        MakeFonts(HIWORD(wp));
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            wchar_t cls[32] = L"";
            GetClassNameW(c, cls, 32);
            if (!_wcsicmp(cls, glass::kCombo)) {
                const int dw = (int)SendMessageW(c, CB_GETDROPPEDWIDTH, 0, 0);
                if (dw > 0) SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)std::lround(dw * g_s / old), 0);
            }
        }
        if (g_tip) SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, Px(380));
        const RECT* r = (const RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();
        MeasureHelp();
        Relayout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        RECT r{0, 0, Px(MinClientW()), Px(kMinClientH)};
        AdjustFrame(r, (DWORD)GetWindowLongW(h, GWL_STYLE));
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_NCCALCSIZE:
        // no title bar: the client starts at the window's top edge (the header with the logo is the caption, so the Mica / the
        // window colour runs through without a seam); the side and bottom resize borders stay. Maximized, the window hangs over
        // the screen edge by its frame on every side: the top is inset like the sides.
        if (wp) {
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
            const LONG top = p->rgrc[0].top, left = p->rgrc[0].left;
            const LRESULT r = DefWindowProcW(h, msg, wp, lp);
            p->rgrc[0].top = top + (IsZoomed(h) ? p->rgrc[0].left - left : 0);
            return r;
        }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NCHITTEST: {
        const LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (r != HTCLIENT) return r;
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(h, &pt);
        RECT rc{};
        GetClientRect(h, &rc);
        if (!IsZoomed(h) && pt.y < Px(6)) return pt.x < Px(12) ? HTTOPLEFT : pt.x >= rc.right - Px(12) ? HTTOPRIGHT : HTTOP;
        // the header (the logo lets the mouse through) and the footer's free space drag the window
        RECT lr{};
        if (HWND logo = Ctl(ID_LOGO)) { GetWindowRect(logo, &lr); MapWindowPoints(nullptr, h, (POINT*)&lr, 2); }
        if (pt.y < lr.bottom + Px(4) || pt.y >= rc.bottom - Px(g_footerH)) return HTCAPTION;
        return r;
    }
    case WM_CLOSE:
        TryClose();
        return 0;
    case WM_DESTROY:
        KillTimer(h, kQueueTimer);
        g_qItems.clear();
        g_qIds.clear();
        g_qRunNow = false;
        KillTimer(h, kHoverTimer);
        KillTimer(h, kCaptureTimer);
        KillTimer(h, kPulseTimer);
        KillTimer(h, kFreeSpaceTimer);
        KillTimer(h, kSavedTimer);
        g_capturing = 0;
        input::SetCapture(false);
        g_done = true;
        g_dlg = nullptr;
        g_tip = nullptr;
        g_hover = nullptr;
        g_shown = nullptr;
        g_scrollY = g_scrollMax = g_viewH = 0;
        g_dim.clear();
        g_footer.clear();
        g_made.clear();
        g_madeTab.clear();
        g_wantHidden.clear();
        g_tabHover = -1;
        g_domeLabels.clear();
        g_qpLabels.clear();
        g_helpMinH = g_helpPad = g_naturalH = 0;
        g_contentEnd = 0;
        g_footerRaised = false;
        PostMessageW(nullptr, WM_NULL, 0, 0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

// BuildControls runs in two passes with the same sequence of Make calls: Create makes every control once (and records it in
// g_made); Move (Relayout: resize / scroll) walks g_made in the same order and only moves / sizes each control (DeferWindowPos).
// Everything that must happen only once (tooltips, items, ranges, the g_dim / g_footer lists) is skipped in the Move pass.
struct Builder {
    enum class Pass { Create, Move };
    HWND parent;
    Pass pass = Pass::Create;
    size_t step = 0;        // Move: index of the next control in g_made
    HDWP dwp = nullptr;     // Move: the batched moves (nullptr = SetWindowPos one by one)
    int dy = 0;             // vertical offset added to every y (-scroll offset for the content, 0 for the footer)
    int tab = -1;           // Create: the tab the next controls belong to (-1 = always shown)
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int hgt, int id, DWORD ex = 0) {
        if (pass == Pass::Move) {
            if (step >= g_made.size()) { ++step; return nullptr; }   // a sequence mismatch: BuildControls logs it
            HWND c = g_made[step++];
            if (dwp) dwp = DeferWindowPos(dwp, c, nullptr, Px(x), Px(y + dy), Px(w), Px(hgt), SWP_NOZORDER | SWP_NOACTIVATE);
            if (!dwp) SetWindowPos(c, nullptr, Px(x), Px(y + dy), Px(w), Px(hgt), SWP_NOZORDER | SWP_NOACTIVATE);
            return c;
        }
        HWND c = CreateWindowExW(ex, GlassClass(cls), text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, Px(x), Px(y + dy), Px(w), Px(hgt), parent,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        if (g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        if (!_wcsicmp(cls, L"EDIT") || !_wcsicmp(cls, L"LISTBOX")) glass::FrameEdit(c);
        g_made.push_back(c);
        g_madeTab.push_back(tab);
        return c;
    }
    void Tip(HWND c, const wchar_t* t) { if (pass == Pass::Create) AddTip(c, t); }
    // Width of `t` in the dialog font (no padding), in layout units.
    int TextW(const wchar_t* t, HFONT font = g_font) {
        HDC dc = GetDC(parent);
        HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
        SIZE s{};
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &s);
        if (old) SelectObject(dc, old);
        ReleaseDC(parent, dc);
        return Lg(s.cx);
    }
    // A row label (text height 16, centred on a 23 px control that starts at y). helpId != 0: hovering the label shows the help of
    // that control (the label keeps the id in GWLP_USERDATA).
    HWND Label(int x, int y, int w, const wchar_t* t, int helpId = 0) {
        HWND c = Make(L"STATIC", t, SS_LEFT | SS_NOPREFIX | (helpId ? SS_NOTIFY : 0), x, y + 4, w, TextH(), -1);
        if (helpId) {
            SetWindowLongPtrW(c, GWLP_USERDATA, helpId);
            Tip(c, HelpText(helpId));
        }
        return c;
    }
    // A hint / caption: a label drawn in the grey hint colour (glass::SetDim, ApplyGlassTheme).
    HWND Hint(int x, int y, int w, const wchar_t* t, int helpId = 0) {
        HWND c = Label(x, y, w, t, helpId);
        if (pass == Pass::Create) g_dim.push_back(c);
        return c;
    }
    void Edit(int id, int x, int y, int w) {
        Tip(Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 23, id, WS_EX_CLIENTEDGE), HelpText(id));
    }
    // Returns the width taken (box + text), so the caller can lay several checks out in a row.
    int Check(int id, int x, int y, const wchar_t* t) {
        const int w = TextW(t) + 24;
        Tip(Make(L"BUTTON", t, WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY, x, y + 1, w, 21, id), HelpText(id));
        return w;
    }
    // Push buttons are 26 px tall and centred on the 23 px row that starts at y.
    void Button(int id, int x, int y, int w, const wchar_t* t, bool def = false) {
        Tip(Make(L"BUTTON", t, WS_TABSTOP | BS_NOTIFY | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y - 2, w, 26, id), HelpText(id));
    }
    int ButtonW(const wchar_t* t) { const int w = TextW(t) + 28; return w < 88 ? 88 : w; }
    // Height of a one-line text control: 16 layout units, or a line of the font if that is taller (fonts do not scale exactly).
    int TextH() { const int l = LineH(); return l > 16 ? l : 16; }
    // Height of one text line in the dialog font (what a multi-line edit / static advances per line), in layout units /
    // in device pixels (the help edit's formatting rectangle).
    int LineH() { return Lg(LineHPx()); }
    int LineHPx() {
        HDC dc = GetDC(parent);
        HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        if (old) SelectObject(dc, old);
        ReleaseDC(parent, dc);
        return tm.tmHeight > 0 ? tm.tmHeight : 16;
    }
    // Group boxes are created first (so the tab order is the creation order) and then sent to the bottom of the z-order, below the
    // controls they surround: every control has WS_CLIPSIBLINGS, so a group box that stayed above its controls would hide them.
    std::vector<HWND> groups;
    void Group(int x, int y, int w, int hgt, const wchar_t* t) { groups.push_back(Make(L"BUTTON", t, BS_GROUPBOX, x, y, w, hgt, -1)); }
    // A check box bound to a config key but not shown (the "YouTube output" drop-down drives it): no WS_VISIBLE, no tab stop, 0x0.
    // Not in g_made (nothing to move): the Move pass does nothing.
    void HiddenCheck(int id) {
        if (pass == Pass::Move) return;
        HWND c = CreateWindowExW(0, glass::kButton, L"", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                                 GetModuleHandleW(nullptr), nullptr);
        if (g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    // The items come from the Field (display labels when it has them): the values written to config.ini are mapped by INDEX.
    void Combo(int id, int x, int y, int w) {
        const Field* f = FindFieldById(id);
        ComboItems(id, x, y, w, f ? (f->labels ? f->labels : f->options) : nullptr);
    }
    // Explicit '|'-separated display items, for a combo that is not a config Field (ID_YTMODE).
    void Combo(int id, int x, int y, int w, const wchar_t* items) { ComboItems(id, x, y, w, items); }
    void ComboItems(int id, int x, int y, int w, const wchar_t* items) {
        HWND c = Make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 23, id);
        Tip(c, HelpText(id));
        if (!items || pass == Pass::Move) return;
        const std::wstring o = items;
        size_t pos = 0;
        int widest = 0;
        while (pos <= o.size()) {
            size_t bar = o.find(L'|', pos);
            if (bar == std::wstring::npos) bar = o.size();
            const std::wstring item = o.substr(pos, bar - pos);
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)item.c_str());
            const int iw = TextW(item.c_str());
            if (iw > widest) widest = iw;
            pos = bar + 1;
        }
        const int dw = widest + 28;   // the glass list: 14 px text inset + 8 px right + the scroll indicator
        SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)Px(dw > w ? dw : w), 0);
    }
};

// Creates (Pass::Create) or moves (Pass::Move) all controls for a client of clientW x clientH, the content scrolled up by
// scrollY; returns the client height the window would need (g_contentH / g_footerH tell where the footer starts) and writes
// the natural content height (help panel at its measured minimum, g_helpMinH) to *naturalH. The footer is pinned to the
// bottom of the client; extra height goes to the Description panel, extra width to the columns (and the two previews).
// Layout: client width >= kLayoutW, margin 12, two columns of group boxes, and inside a group ONE grid: labels in a fixed left
// column, every control starting at the same x (`cx`) in the second column; rows are 30 px apart, edits / combos 23 px tall,
// buttons 26 px tall and at least 88 px wide. The sequence of Make calls must not depend on the pass or the size.
int BuildControls(HWND dlg, Builder::Pass pass, int clientW, int clientH, int scrollY, int* naturalH) {
    const bool create = pass == Builder::Pass::Create;
    clientW = (int)(clientW / g_s);   // device pixels -> layout units
    clientH = (int)(clientH / g_s);
    Builder b{dlg, pass};
    auto keep = [&](std::vector<HWND>& v, HWND c) { if (create) v.push_back(c); return c; };
    g_footerH = kFooterH;
    if (create) {
        g_made.clear();
        g_madeTab.clear();
        g_wantHidden.clear();
        g_dim.clear();
        g_footer.clear();
        g_domeLabels.clear();
        g_qpLabels.clear();
        g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (g_tip) {
            SetWindowPos(g_tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, Px(380));
            SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(30000, 0));
            if (glass::Current().dark) SetWindowTheme(g_tip, L"DarkMode_Explorer", nullptr);
        }
    } else {
        b.dwp = BeginDeferWindowPos((int)g_made.size());
    }
    b.dy = -scrollY;
    // The header (logo, update line), the tab strip, then one page per tab: two columns of group boxes (Recording | Recording
    // Indicator, Video | Stereo 3D, Export | Conversions, App | Advanced). Every group is as tall as the tallest one (Export),
    // so the Description and the footer below stay put when the tab changes. setCol() moves the grid to a column: label
    // column at lx, controls at cx.
    const int margin = 12, colGap = 12, fullW = clientW - 2 * margin, colW = (fullW - colGap) / 2;
    const int colWMin = (kLayoutW - 2 * margin - colGap) / 2;
    const int extraW = colW > colWMin ? colW - colWMin : 0;   // the layout preview grows with the column
    int gx = margin, gw = colW, lx = 0, cx = 0, cw = 0;
    const int labelW = 108;
    auto setCol = [&](int x, int w) { gx = x; gw = w; lx = gx + 12; cx = gx + 132; cw = gx + gw - 12 - cx; };
    setCol(margin, colW);
    const int pitch = 30, top = 22, gap = 8;
    auto groupH = [&](int rows) { return top + rows * pitch + 2; };
    const int unitGap = 6;
    const int kExportRows = 11;                      // the tallest group: it sets the page height
    const int pageH = groupH(kExportRows) + 6;
    int y = 6;

    // ---- header: the logo (mark + wordmark + tagline, owner-drawn by DrawLogo) and, at its right end, the "update available"
    // line (hidden until a newer release is known)
    {
        const int logoW = 32 + 6 + b.TextW(L"VR EyeCapture", g_logoFont) + 16 + b.TextW(L"Stereo 3D recorder for VR games") + 2;
        b.Make(L"STATIC", L"", SS_OWNERDRAW, gx, y, logoW, 32, ID_LOGO);
        const int ux = gx + logoW + 24;
        HWND ul = b.Make(WC_LINK, L"", WS_TABSTOP | LWS_RIGHT, ux, y + (32 - 16) / 2, gx + fullW - ux, b.TextH(), ID_UPDATELINK);
        b.Tip(ul, HelpText(ID_UPDATELINK));
        if (create) SetShown(ul, false);
    }
    y += 32 + 8;

    // ---- the tab strip (DrawTabs / TabsProc)
    {
        HWND ts = b.Make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY | WS_TABSTOP, gx, y, fullW, 40, ID_TABS);
        b.Tip(ts, HelpText(ID_TABS));
        if (create && ts) SetWindowSubclass(ts, TabsProc, 1, 0);
    }
    y += 40 + 10;
    const int yTop = y;

    // Rows that carry two settings (Audio + Device, Codec + Encoder preset, Resolution + Colour depth, Frame rate limit +
    // Constant frame rate, Projection + Sharpen, Head smoothing + FOV scale) keep the groups short. Texts that depend on
    // the room are picked for the NARROWEST layout (cwMin), so the Create and Move passes always agree.
    const int cwMin = colWMin - 12 - 132;
    auto pickText = [&](int room, std::initializer_list<const wchar_t*> texts, int pad) {
        for (const wchar_t* t : texts) if (b.TextW(t) + pad <= room) return t;
        return *(texts.end() - 1);
    };
    // width of a drop-down that shows its widest item (+ `extra`, e.g. a Custom item) without cutting it (CheckTextFit's rule)
    auto comboW = [&](int id, const wchar_t* extra) {
        const Field* f = FindFieldById(id);
        int w = extra ? b.TextW(extra) : 0;
        const std::wstring o = f ? (f->labels ? f->labels : f->options) : L"";
        size_t pos = 0;
        while (pos <= o.size()) {
            size_t bar = o.find(L'|', pos);
            if (bar == std::wstring::npos) bar = o.size();
            const int t = b.TextW(o.substr(pos, bar - pos).c_str());
            if (t > w) w = t;
            pos = bar + 1;
        }
        return w + Lg(GetSystemMetricsForDpi(SM_CXVSCROLL, Dpi())) + 14;
    };

    // ================================ tab 0: Recording
    b.tab = 0;
    // ---- Recording
    b.Group(gx, y, gw, pageH, L"Recording");
    int r = y + top;
    b.Label(lx, r, labelW, L"Hotkey:", ID_HOTKEY);
    {   // one row: the keyboard / pad hotkey, then the VR controller combination (vr_hotkey) at the right end
        const int vw = comboW(ID_VRHOTKEY, nullptr), vlw = b.TextW(L"Controllers:") + 6;
        const int vx = cx + cw - vw;
        b.Edit(ID_HOTKEY, cx, r, vx - vlw - 12 - cx);
        b.Label(vx - vlw - 4, r, vlw, L"Controllers:", ID_VRHOTKEY);
        b.Combo(ID_VRHOTKEY, vx, r, vw);
    }
    r += pitch;
    {   // wide enough for the armed texts ("Press a key... (Esc cancels)")
        int bw = b.ButtonW(L"Press a key... (Esc cancels)");
        const int bw2 = b.ButtonW(L"Press to add... (Esc cancels)");
        if (bw2 > bw) bw = bw2;
        b.Button(ID_HOTKEY_PICK, cx, r, bw, L"Capture...");
        b.Button(ID_HOTKEY_ADD, cx + bw + 8, r, bw, L"Add...");
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Output folder:", ID_OUTDIR);
    b.Edit(ID_OUTDIR, cx, r, cw - 88 - 8);
    b.Button(ID_BROWSE, cx + cw - 88, r, 88, L"Browse...");
    r += pitch;
    b.Label(lx, r, labelW, L"Free space:", ID_OUTFREE);
    SetWindowLongPtrW(b.Hint(cx, r, cw, L"", ID_OUTFREE), GWLP_ID, ID_OUTFREE);   // the hint is updated by UpdateFreeSpace
    r += pitch;
    b.Label(lx, r, labelW, L"Audio:", ID_AUDIO);
    {   // one row: the audio check, then "Device:" + the device field (its "empty = default" note is the field's cue banner)
        const int aw = b.Check(ID_AUDIO, cx, r, L"Record system audio");
        const int dx = cx + aw + 16, dlw = b.TextW(L"Device:") + 6;
        b.Label(dx, r, dlw, L"Device:", ID_AUDIODEV);
        b.Edit(ID_AUDIODEV, dx + dlw + 2, r, cx + cw - (dx + dlw + 2));
        if (create) SendMessageW(GetDlgItem(dlg, ID_AUDIODEV), EM_SETCUEBANNER, FALSE, (LPARAM)L"Empty = default playback device");
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Overlays:", ID_OVERLAYS);
    b.Check(ID_OVERLAYS, cx, r, L"Include overlay layers (menus, mod panels)");
    r += pitch;
    b.Label(lx, r, labelW, L"Beeps:", ID_BEEPS);
    b.Check(ID_BEEPS, cx, r, L"Beep in the headset when recording starts / stops");

    // ---- Recording Indicator (the fields, then the live preview of the view below them: square, centred in the group)
    setCol(margin + colW + colGap, colW);
    b.Group(gx, y, gw, pageH, L"Recording Indicator");
    r = y + top;
    b.Label(lx, r, labelW, L"Indicator:", ID_INDICATOR);
    {   // one row: the dot itself, then its violet "OBS is watching" state (indicator_live)
        const int iw = b.Check(ID_INDICATOR, cx, r, L"Show in headset");
        b.Check(ID_INDLIVE, cx + iw + 16, r, L"Violet for OBS");
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Size:", ID_INDSIZE);
    b.Edit(ID_INDSIZE, cx, r, 60);
    b.Label(cx + 60 + unitGap, r, 20, L"m", ID_INDSIZE);
    r += pitch;
    b.Label(lx, r, labelW, L"Opacity:", ID_INDOPACITY);
    b.Edit(ID_INDOPACITY, cx, r, 60);
    b.Hint(cx + 60 + 8, r, 100, L"0.0 – 1.0", ID_INDOPACITY);
    r += pitch;
    b.Label(lx, r, labelW, L"Position (m):", ID_INDX);
    {
        int x = cx;
        b.Label(x, r, 12, L"X", ID_INDX);
        b.Edit(ID_INDX, x + 16, r, 52);
        x += 84;
        b.Label(x, r, 12, L"Y", ID_INDY);
        b.Edit(ID_INDY, x + 16, r, 52);
        x += 84;
        b.Label(x, r, 12, L"Z", ID_INDZ);
        b.Edit(ID_INDZ, x + 16, r, 52);
    }
    r += pitch;
    {
        const int room = y + pageH - 12 - (r + 2);
        int prevSize = room < gw - 24 ? room : gw - 24;
        if (prevSize > 220) prevSize = 220;
        HWND pv = b.Make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, gx + (gw - prevSize) / 2, r + 2 + (room - prevSize) / 2, prevSize,
                         prevSize, ID_INDPREVIEW);
        b.Tip(pv, HelpText(ID_INDPREVIEW));
        if (create && pv) SetWindowSubclass(pv, PreviewProc, 1, 0);   // click / drag / wheel places the dot
    }

    // ================================ tab 1: Video
    b.tab = 1;
    setCol(margin, colW);
    // ---- Video (the QP edit is greyed unless Rate control is Constant QP, the Bitrate drop-down greyed for it)
    b.Group(gx, y, gw, pageH, L"Video");
    r = y + top;
    b.Label(lx, r, labelW, L"Codec:", ID_CODEC);
    {   // one row: Codec, then "Encoder preset:" (quality_vs_speed), which takes the rest of the row
        const int ccw = comboW(ID_CODEC, nullptr), pcw = comboW(ID_PRESET, L"Custom (100)");
        const int px = cx + ccw + 16;
        const wchar_t* pl = pickText(cwMin - (ccw + 16) - pcw, {L"Encoder preset:", L"Preset:"}, 6);
        const int plw = b.TextW(pl) + 6;
        b.Combo(ID_CODEC, cx, r, ccw);
        b.Label(px, r, plw, pl, ID_PRESET);
        b.Combo(ID_PRESET, px + plw, r, cx + cw - (px + plw));
    }
    r += pitch;
    g_bitrateLabel = b.Label(lx, r, labelW, L"Bitrate:", ID_BITRATE);
    b.Combo(ID_BITRATE, cx, r, 190);
    SetWindowLongPtrW(b.Hint(cx + 190 + 8, r, cw - 198, L"", ID_BITRATEHINT), GWLP_ID, ID_BITRATEHINT);   // updated by UpdateBitrateHint
    r += pitch;
    b.Label(lx, r, labelW, L"Rate control:", ID_RATECONTROL);
    b.Combo(ID_RATECONTROL, cx, r, 190);
    {   // same row: "QP:" + the 60 px edit + its range hint
        const int qx = cx + 190 + 12, qw = b.TextW(L"QP:") + 6;
        keep(g_qpLabels, b.Label(qx, r, qw, L"QP:", ID_QP));
        b.Edit(ID_QP, qx + qw + 2, r, 60);
        const int hx = qx + qw + 2 + 60 + 8;
        keep(g_qpLabels, b.Hint(hx, r, cx + cw - hx, L"0 = best, 51 = worst", ID_QP));
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Resolution:", ID_MAXWIDTH);
    b.Combo(ID_MAXWIDTH, cx, r, 190);
    {   // same row: "Colour depth:" (bit_depth) takes the rest of the row
        const int bx = cx + 190 + 16;
        const wchar_t* bl = pickText(cwMin - (190 + 16) - comboW(ID_BITDEPTH, nullptr), {L"Colour depth:", L"Colour:"}, 6);
        const int blw = b.TextW(bl) + 6;
        b.Label(bx, r, blw, bl, ID_BITDEPTH);
        b.Combo(ID_BITDEPTH, bx + blw, r, cx + cw - (bx + blw));
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Frame rate limit:", ID_MAXFPS);
    b.Edit(ID_MAXFPS, cx, r, 60);
    b.Label(cx + 60 + unitGap, r, 24, L"fps", ID_MAXFPS);
    {   // same row, after the "0 = unlimited" hint: the constant_frame_rate check
        const int hx = cx + 60 + unitGap + 30, hw = b.TextW(L"0 = unlimited") + 4;
        b.Hint(hx, r, hw, L"0 = unlimited", ID_MAXFPS);
        const int fx = hx + hw + 16;
        b.Check(ID_CFR, fx, r, pickText(cwMin - (fx - cx), {L"Constant frame rate (for video editors such as DaVinci Resolve)",
                                                             L"Constant frame rate (for video editors)", L"Constant frame rate"}, 24));
    }

    // ---- Stereo 3D (Output layout lives in the Export group: it only shapes the files made for YouTube, or the recording
    // itself with record_for_youtube). Dome fill / Fade after / Shared view are greyed unless Projection is VR180 / VR360.
    setCol(margin + colW + colGap, colW);
    b.Group(gx, y, gw, pageH, L"Stereo 3D");
    r = y + top;
    b.Label(lx, r, labelW, L"Projection:", ID_PROJECTION);
    {   // one row: Projection, then "Sharpen:" (sharpen; every layout, most useful for the domes)
        const int pw = comboW(ID_PROJECTION, nullptr), sx = cx + pw + 16, slw = b.TextW(L"Sharpen:") + 6;
        b.Combo(ID_PROJECTION, cx, r, pw);
        b.Label(sx, r, slw, L"Sharpen:", ID_SHARPEN);
        b.Combo(ID_SHARPEN, sx + slw, r, cx + cw - (sx + slw));
    }
    r += pitch;
    keep(g_domeLabels, b.Label(lx, r, labelW, L"Dome fill:", ID_FILL));
    b.Combo(ID_FILL, cx, r, 160);
    {
        const int fx = cx + 160 + 16, fw = b.TextW(L"Fade after:") + 8;
        keep(g_domeLabels, b.Label(fx, r, fw, L"Fade after:", ID_FILLFADE));
        b.Edit(ID_FILLFADE, fx + fw + 2, r, 50);
        keep(g_domeLabels, b.Label(fx + fw + 2 + 50 + unitGap, r, 14, L"s", ID_FILLFADE));
        // same row, after the "s": the dome_shared_view check
        const int sx = fx + fw + 2 + 50 + unitGap + 14 + 16;
        b.Check(ID_SHAREDVIEW, sx, r, L"Shared view");
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Options:", ID_SYMFOV);
    {
        int x = cx;
        x += b.Check(ID_SYMFOV, x, r, L"Symmetric field of view") + 8;
        x += b.Check(ID_SWAP, x, r, L"Swap eyes") + 8;
        b.Check(ID_ROLL, x, r, L"Level horizon (roll lock)");
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Head smoothing:", ID_SMOOTH);
    {   // one row: the slider (takes the rest), its value, then the field of view scale (it only matters with smoothing on)
        const wchar_t* fl = pickText(cwMin - 200 - (8 + 44 + 16) - 60, {L"Field of view scale:", L"FOV scale:"}, 6);
        const int flw = b.TextW(fl) + 6;
        const int sw = cw - (8 + 44 + 16) - flw - 60;
        HWND tb = b.Make(L"msctls_trackbar32", L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, cx, r - 3, sw, 28, ID_SMOOTH);
        b.Tip(tb, HelpText(ID_SMOOTH));
        if (create) {
            SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(0, 20));
            SendMessageW(tb, TBM_SETTICFREQ, 2, 0);
            SendMessageW(tb, TBM_SETPAGESIZE, 0, 2);
            SendMessageW(tb, TBM_SETLINESIZE, 0, 1);
        }
        b.Make(L"STATIC", L"0.00", SS_LEFT | SS_NOPREFIX, cx + sw + 8, r + 4, 44, b.TextH(), ID_SMOOTHVAL);
        const int fx = cx + sw + 8 + 44 + 16;
        b.Label(fx, r, flw, fl, ID_FOVSCALE);
        b.Edit(ID_FOVSCALE, fx + flw, r, 60);
    }

    // ================================ tab 2: Export
    b.tab = 2;
    setCol(margin, colW);
    // ---- Export (fields on the left, the live layout preview on the right). The preview is 213 px wide at the minimum width and
    // grows with the column by up to 40 px (its text lines need that room; DrawLayoutPreview draws at fixed pixel positions, so
    // more would only add empty space); it stays right-aligned in the column and the combos left of it (`ecw`) take the rest.
    const int lpW = 213 + (extraW < 40 ? extraW : 40), lpH = 140;
    const int ecw = gx + gw - 12 - lpW - 12 - cx;   // combo width that leaves a gap before the preview
    // rows: YouTube output, Convert, 8K bitrate, 8K codec, the 2D check (left of the preview: their texts fit `ecw`), Keep MP4,
    // Output layout, Eye crop (below the preview: the layout items are wider than `ecw`), the record_for_youtube check, the
    // tag_3d check, the two conversion buttons
    b.Group(gx, y, gw, pageH, L"Export");
    r = y + top;
    b.Label(lx, r, labelW, L"YouTube output:", ID_YTMODE);
    b.Combo(ID_YTMODE, cx, r, ecw, L"3D MKV|8K 3D MKV|3D MKV + 8K 3D MKV|None");
    r += pitch;
    b.Label(lx, r, labelW, L"Convert:", ID_POSTWHEN);
    b.Combo(ID_POSTWHEN, cx, r, ecw);
    r += pitch;
    g_yt8kBitrateLabel = b.Label(lx, r, labelW, L"8K bitrate:", ID_YT8KBITRATE);
    b.Combo(ID_YT8KBITRATE, cx, r, ecw);
    r += pitch;
    g_yt8kCodecLabel = b.Label(lx, r, labelW, L"8K codec:", ID_YT8KCODEC);
    b.Combo(ID_YT8KCODEC, cx, r, ecw);
    r += pitch;
    b.Label(lx, r, labelW, L"Options:", ID_POSTFLAT);
    b.Check(ID_POSTFLAT, cx, r, L"Also create a 2D (flat) video");
    r += pitch;
    b.Check(ID_KEEPMP4, cx, r, L"Keep original MP4");
    r += pitch;
    g_layoutLabel = b.Label(lx, r, labelW, L"Output layout:", ID_LAYOUT3D);
    b.Combo(ID_LAYOUT3D, cx, r, 240);
    r += pitch;
    b.Label(lx, r, labelW, L"Eye crop:", ID_EYECROP);
    b.Combo(ID_EYECROP, cx, r, cw < 330 ? cw : 330);   // below the preview: room for the 4:3pad label
    r += pitch;
    b.Label(lx, r, labelW, L"Recording:", ID_RECORDYT);
    b.Check(ID_RECORDYT, cx, r, L"Record directly in the YouTube layout (no re-encode)");
    r += pitch;
    b.Label(lx, r, labelW, L"3D tag:", ID_TAG3DMP4);
    b.Check(ID_TAG3DMP4, cx, r, L"Write the 3D tag into the recorded MP4");
    r += pitch;
    {   // below the preview: the whole control column (Convert = add and start now; Add to queue = add and wait for Start)
        const int bw = (cw - 8) / 2;
        b.Button(ID_CONVERTPICK, cx, r, bw, L"Convert a recording...");
        b.Button(ID_ADDQUEUE, cx + bw + 8, r, cw - bw - 8, L"Add to queue...");
    }
    {   // the frame-rate warning: sits on the group box's top edge right of the "Export" title (opaque, so it interrupts the line);
        // empty and hidden until UpdateFpsWarning finds a game at an uneven rate
        HWND wl = b.Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_NOTIFY, gx + 62, y, gw - 62 - 12, b.TextH(), ID_FPSWARN);
        b.Tip(wl, HelpText(ID_FPSWARN));
        if (create) SetShown(wl, false);
    }
    b.HiddenCheck(ID_POSTMKV);     // bound to post_mkv / post_yt8k, set from / read into the drop-down
    b.HiddenCheck(ID_POSTYT8K);
    b.Tip(b.Make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, gx + gw - 12 - lpW, y + top, lpW, lpH, ID_LAYOUTPREVIEW),
          HelpText(ID_LAYOUTPREVIEW));

    // ---- Conversions (right column, top to bottom): the running job (three status lines: file, format, step / progress /
    // time left; progress bar; Pause / Cancel), then the waiting list (multi-select, scrolls sideways for long rows) with
    // Start / Start all / Remove beside it. Filled by UpdateQueueView.
    setCol(margin + colW + colGap, colW);
    b.Group(gx, y, gw, pageH, L"Conversions");
    {
        const int lh = b.LineH();
        const int stH = 3 * lh, pbY = stH + 6, btY = pbY + 14 + 8;
        r = y + top;
        b.Label(lx, r, labelW, L"Running:", ID_QSTATUS);
        b.Tip(b.Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_NOTIFY, cx, r + 2, cw, stH, ID_QSTATUS), HelpText(ID_QSTATUS));
        HWND pb = b.Make(PROGRESS_CLASSW, L"", PBS_SMOOTH, cx, r + 2 + pbY, cw, 14, ID_QPROGRESS);
        if (create) SendMessageW(pb, PBM_SETRANGE32, 0, 100);
        b.Tip(pb, HelpText(ID_QPROGRESS));
        b.Button(ID_QPAUSE, cx, r + 2 + btY, 96, L"Pause");
        b.Button(ID_QCANCEL, cx + 96 + 8, r + 2 + btY, 96, L"Cancel");
        r += btY + 26 + 18;
        b.Label(lx, r, labelW, L"Waiting:", ID_QLIST);
        int qbW = b.ButtonW(L"Start all");
        for (const wchar_t* t : {L"Start", L"Remove"}) if (b.ButtonW(t) > qbW) qbW = b.ButtonW(t);
        const int lbW = cw - qbW - 8, listH = y + pageH - 12 - r;
        b.Tip(b.Make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, cx, r,
                     lbW, listH, ID_QLIST, WS_EX_CLIENTEDGE),
              HelpText(ID_QLIST));
        b.Button(ID_QSTART, cx + lbW + 8, r + 2, qbW, L"Start");
        b.Button(ID_QSTARTALL, cx + lbW + 8, r + 2 + 34, qbW, L"Start all");
        b.Button(ID_QREMOVE, cx + lbW + 8, r + 2 + 68, qbW, L"Remove");
    }

    // ================================ tab 3: App
    b.tab = 3;
    setCol(margin, colW);
    // ---- App: the look, startup, updates and the config file
    b.Group(gx, y, gw, pageH, L"App");
    r = y + top;
    b.Label(lx, r, labelW, L"Theme:", ID_THEME);
    b.Combo(ID_THEME, cx, r, comboW(ID_THEME, nullptr));
    r += pitch;
    b.Label(lx, r, labelW, L"Startup:", ID_AUTOSTART);
    b.Check(ID_AUTOSTART, cx, r, L"Start with Windows");
    r += pitch;
    b.Label(lx, r, labelW, L"Updates:", ID_CHECKUPDATES);
    {   // check_updates + a "Check now" button (a manual check works even when the automatic one is off)
        const int cuw = b.Check(ID_CHECKUPDATES, cx, r, L"Check for updates automatically");
        b.Button(ID_UPDATENOW, cx + cuw + 12, r, b.ButtonW(L"Check now"), L"Check now");
    }
    r += pitch;
    b.Label(lx, r, labelW, L"Config file:");
    keep(g_dim, b.Make(L"STATIC", g_path.c_str(), SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, cx, r + 4, cw, b.TextH(), -1));

    // ---- Advanced
    setCol(margin + colW + colGap, colW);
    b.Group(gx, y, gw, pageH, L"Advanced");
    r = y + top;
    b.HiddenCheck(ID_REQTRAY);   // require_tray stays a config.ini key (default 1); the tray is required anyway, so no checkbox
    b.Label(lx, r, labelW, L"SteamVR games:", ID_SVRMODE);
    b.Combo(ID_SVRMODE, cx, r, comboW(ID_SVRMODE, nullptr));
    r += pitch;
    b.Label(lx, r, labelW, L"Log level:", ID_LOGLEVEL);
    b.Combo(ID_LOGLEVEL, cx, r, 160);
    r += pitch;
    b.Label(lx, r, labelW, L"Tools:", ID_OPENLOG);
    {   // moved here from the tray menu: Open log, Tag as 3D (a small menu: side by side / top-bottom), Register / Unregister
        const int w1 = b.ButtonW(L"Open log"), w2 = b.ButtonW(L"Tag as 3D...");
        int w3 = b.ButtonW(L"Register for VR games...");
        const int w3b = b.ButtonW(L"Unregister from VR games...");
        if (w3b > w3) w3 = w3b;
        b.Button(ID_OPENLOG, cx, r, w1, L"Open log");
        b.Button(ID_TAG3D, cx + w1 + 8, r, w2, L"Tag as 3D...");
        b.Button(ID_REGLAYER, cx + w1 + w2 + 16, r, w3, L"Register for VR games...");
    }

    b.tab = -1;
    y = yTop + pageH + gap;
    setCol(margin, fullW);

    // ---- Description panel (follows the mouse / keyboard focus, see Hover / ShowHelp). At least g_helpMinH tall (the longest
    // help text, MeasureHelp; 5 lines before it ran); a window taller than the content gives all its extra height to it.
    const int lh = b.LineH();
    const int helpMin = g_helpMinH > 0 ? g_helpMinH : 5 * lh;
    const int naturalContentH = y + (20 + helpMin + 10) + gap - 4;   // = g_contentH below with helpH = helpMin
    const int avail = (clientH - g_footerH) - naturalContentH;
    const int helpH = helpMin + (avail > 0 ? avail : 0);
    const int descH = 20 + helpH + 10;
    b.Group(gx, y, gw, descH, L"Description");
    {
        HWND he = b.Make(L"EDIT", kOverview, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, lx, y + 20, gw - 24, helpH, ID_HELPTEXT);
        // Move pass: apply the queued moves now, so the edit has its new size (which resets its formatting rectangle) below
        if (!create && b.dwp) {
            EndDeferWindowPos(b.dwp);
            b.dwp = BeginDeferWindowPos(8);
        }
        // the formatting rectangle: whole lines from the top, so the last visible line is never cut in half
        RECT fr{};
        if (he) SendMessageW(he, EM_GETRECT, 0, (LPARAM)&fr);   // device pixels
        const int frH = Lg(fr.bottom - fr.top), lhPx = b.LineHPx();
        if (create) g_helpPad = helpH > frH ? helpH - frH : 0;
        const int lines = (fr.bottom - fr.top) / lhPx;
        if (he && lines >= 1) {
            fr.bottom = fr.top + lines * lhPx;
            SendMessageW(he, EM_SETRECTNP, 0, (LPARAM)&fr);
        }
    }
    if (create) g_shown = kOverview;
    y += descH + gap;

    // ---- footer: an opaque strip (so scrolled content never shows through when the window is shorter than the content),
    // one short note and the buttons. It is pinned to the bottom of the client and does not scroll.
    g_contentH = y - 4;
    b.dy = 0;
    const int fy = clientH - g_footerH;
    const int by = fy + 32;
    const int wOpen = b.ButtonW(L"Open config.ini"), wDef = b.ButtonW(L"Restore Defaults"), wSvr = b.ButtonW(L"SteamVR games..."), wOk = 96;
    if (create) g_contentEnd = g_made.size();   // g_made[0 .. g_contentEnd) = the content, the rest = the footer
    b.Button(ID_OPENINI, gx, by, wOpen, L"Open config.ini");
    b.Button(ID_DEFAULTS, gx + wOpen + 8, by, wDef, L"Restore Defaults");
    b.Button(ID_OPENSTEAMVR, gx + wOpen + wDef + 16, by, wSvr, L"SteamVR games...");
    EnableWindow(GetDlgItem(dlg, ID_OPENSTEAMVR), g_openSteamVr ? TRUE : FALSE);
    b.Button(ID_SAVE, gx + gw - 3 * wOk - 16, by, wOk, L"Save", true);
    b.Button(ID_MINIMIZE, gx + gw - 2 * wOk - 8, by, wOk, L"Minimize");
    b.Button(ID_CANCEL, gx + gw - wOk, by, wOk, L"Exit");
    if (create) for (int id : {ID_OPENINI, ID_DEFAULTS, ID_OPENSTEAMVR, ID_SAVE, ID_MINIMIZE, ID_CANCEL}) g_footer.push_back(GetDlgItem(dlg, id));
    const wchar_t* const noteText = L"Changes apply to the next recording. Keyboard hotkeys and the indicator require a game restart.";
    const int noteW = b.TextW(noteText) + 4;
    HWND note = b.Make(L"STATIC", noteText, SS_LEFT | SS_NOPREFIX, gx, fy + 9, noteW, b.TextH(), -1);
    keep(g_dim, note);
    keep(g_footer, note);
    // the project link and the copyright / licence line, right-aligned on the note's row (above the background strip)
    HWND about = b.Make(WC_LINK,
        L"<a href=\"https://github.com/JRDevo/VR-EyeCapture\">github.com/JRDevo/VR-EyeCapture</a> · Copyright (c) 2026 JRDevo · "
        L"<a href=\"https://github.com/JRDevo/VR-EyeCapture/blob/master/LICENSE.md\">License: noncommercial, recordings are yours</a>",
        WS_TABSTOP | LWS_RIGHT, gx + noteW + 12, fy + 9, gw - noteW - 12, b.TextH(), ID_ABOUTLINK);
    b.Tip(about, HelpText(ID_ABOUTLINK));
    keep(g_footer, about);
    // created last = lowest of the footer: it only paints the background behind the note and the buttons
    keep(g_footer, b.Make(L"STATIC", L"", SS_LEFT, 0, fy, clientW + 2, g_footerH + 2, kFooterBgId));
    if (create) {
        for (HWND g : b.groups) SetWindowPos(g, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_footerRaised = false;   // the footer stays below the content (Tab order) until scrolling needs it on top (RaiseFooter)
    } else {
        if (b.dwp) EndDeferWindowPos(b.dwp);
        b.dwp = nullptr;
        if (b.step != g_made.size())
            LOGE("settings: layout pass made %zu controls, the window has %zu (BuildControls must make the same calls in every pass)",
                 b.step, g_made.size());
    }
    if (naturalH) *naturalH = naturalContentH;
    return g_contentH + g_footerH;
}

// The window's fonts at `dpi` (the message font and its variants, the logo / tab / icon fonts). Replaces the old ones: every
// control of the window gets the new message font.
void MakeFonts(UINT dpi) {
    const HFONT oldFonts[] = {g_font, g_odFont, g_odFontLg, g_logoFont, g_tabFont, g_iconFont};
    g_font = g_odFont = g_odFontLg = g_logoFont = g_tabFont = g_iconFont = nullptr;
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW gf = ncm.lfMessageFont;
        gf.lfQuality = ANTIALIASED_QUALITY;
        g_odFont = CreateFontIndirectW(&gf);
        gf.lfHeight = (LONG)std::lround(gf.lfHeight / g_s);
        g_odFontLg = CreateFontIndirectW(&gf);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -Px(20);
        lf.lfWeight = FW_BOLD;
        lf.lfQuality = ANTIALIASED_QUALITY;   // drawn straight onto the owner-drawn logo (glass two-pass alpha)
        g_logoFont = CreateFontIndirectW(&lf);
        LOGFONTW tf = ncm.lfMessageFont;
        tf.lfWeight = FW_SEMIBOLD;
        tf.lfQuality = ANTIALIASED_QUALITY;
        g_tabFont = CreateFontIndirectW(&tf);
        // the tab icons: the Windows 11 icon font, else the Windows 10 one (same code points); none = text-only tabs
        for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
            LOGFONTW icf{};
            icf.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 4;
            icf.lfWeight = FW_NORMAL;
            icf.lfCharSet = DEFAULT_CHARSET;
            icf.lfQuality = ANTIALIASED_QUALITY;
            wcscpy_s(icf.lfFaceName, face);
            HFONT f = CreateFontIndirectW(&icf);
            wchar_t got[LF_FACESIZE] = L"";
            HDC dc = GetDC(nullptr);
            HGDIOBJ old = SelectObject(dc, f);
            GetTextFaceW(dc, LF_FACESIZE, got);
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
            if (!_wcsicmp(got, face)) { g_iconFont = f; break; }
            DeleteObject(f);
        }
    }
    if (g_dlg)
        for (HWND c = GetWindow(g_dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (HFONT f : oldFonts) if (f) DeleteObject(f);
}

// The height the help edit needs for the longest help text (kOverview and every kHelp text) at the current (narrowest) width:
// DrawText DT_CALCRECT in the dialog font and the edit's own line count (its wrapping is the ground truth), the larger one.
// Sets g_helpMinH; run right after the Create pass (client kLayoutW wide).
void MeasureHelp() {
    HWND he = Ctl(ID_HELPTEXT);
    if (!he) return;
    Builder b{g_dlg};
    const int lh = b.LineHPx();   // device pixels, like the edit's formatting rectangle
    RECT fr{};
    SendMessageW(he, EM_GETRECT, 0, (LPARAM)&fr);
    const int width = fr.right - fr.left;
    HDC dc = GetDC(g_dlg);
    HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
    int maxLines = 0, maxId = -1;
    auto measure = [&](int id, const wchar_t* t) {
        RECT r{0, 0, width, 0};
        DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        int lines = (r.bottom + lh - 1) / lh;
        SetWindowTextW(he, t);
        const int el = (int)SendMessageW(he, EM_GETLINECOUNT, 0, 0);
        if (el > lines) lines = el;
        if (lines > maxLines) { maxLines = lines; maxId = id; }
    };
    measure(0, kOverview);
    for (const Help& h : kHelp) measure(h.id, h.text);
    if (old) SelectObject(dc, old);
    ReleaseDC(g_dlg, dc);
    SetWindowTextW(he, kOverview);
    g_shown = kOverview;
    g_helpMinH = Lg(maxLines * lh) + g_helpPad;
    LOGI("settings: description panel: %d lines (longest help text id %d)", maxLines, maxId);
}

// While the content scrolls, the footer must be above it (z-order: the first created is the topmost; keep its order) -- that
// also puts the footer first in the Tab order, so without scrolling it goes back to where it was created: right after the last
// content control, in creation order. Only on a state change.
void RaiseFooter(bool raise) {
    if (raise == g_footerRaised || g_footer.empty()) return;
    g_footerRaised = raise;
    if (raise) {
        for (size_t i = g_footer.size(); i-- > 0;) SetWindowPos(g_footer[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    HWND after = g_contentEnd > 0 && g_contentEnd <= g_made.size() ? g_made[g_contentEnd - 1] : HWND_TOP;
    for (HWND f : g_footer) {
        SetWindowPos(f, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        after = f;
    }
}

// Places every control for the current client size and scroll offset (WM_SIZE, ScrollContent): shows / hides the vertical
// scroll bar when the natural content height does not fit, then runs the Move pass of BuildControls.
void Relayout() {
    static bool inLayout = false;   // ShowScrollBar sends a nested WM_SIZE
    if (!g_dlg || g_made.empty() || inLayout) return;
    inLayout = true;
    for (int i = 0; i < 3; ++i) {
        RECT rc{};
        GetClientRect(g_dlg, &rc);
        const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;   // device pixels
        const int chL = (int)(ch / g_s);
        const int view = chL - g_footerH;
        const bool scroll = g_naturalH > view;
        g_viewH = view;
        g_scrollMax = scroll ? g_naturalH - view : 0;
        g_scrollY = g_scrollY < 0 ? 0 : (g_scrollY > g_scrollMax ? g_scrollMax : g_scrollY);
        const bool shown = (GetWindowLongW(g_dlg, GWL_STYLE) & WS_VSCROLL) != 0;
        if (shown != scroll && i < 2) {
            ShowScrollBar(g_dlg, SB_VERT, scroll);
            continue;   // the client width changed: read it again
        }
        RaiseFooter(scroll);
        int natural = 0;
        BuildControls(g_dlg, Builder::Pass::Move, cw, ch, g_scrollY, &natural);
        g_naturalH = natural;
        if (scroll) {
            SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
            si.nMin = 0;
            si.nMax = g_naturalH + g_footerH - 1;
            si.nPage = (UINT)chL;
            si.nPos = g_scrollY;
            SetScrollInfo(g_dlg, SB_VERT, &si, TRUE);
        }
        break;
    }
    inLayout = false;
    RedrawWindow(g_dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// Measures the text of every visible label, check box, button, group box and combo item with the dialog font (GetTextExtentPoint32
// / DrawText DT_CALCRECT) against its control and logs each one that would be cut ("settings: text does not fit: ..."), then one
// summary line "settings: text fit check: N controls, M cut" (tests require M = 0). Run once the window is filled (after
// LoadFromFile), so the drop-downs and dynamic hints hold real values. Statics with an ellipsis style, owner-drawn controls,
// edits and the list box (it scrolls sideways) are skipped. The Description panel is checked separately: every help text must
// fit its visible lines (EM_GETLINECOUNT against the formatting rectangle), each counts as one control.
void CheckTextFit() {
    if (!g_dlg) return;
    HDC dc = GetDC(g_dlg);
    HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
    auto textW = [&](const std::wstring& t) { SIZE z{}; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &z); return (int)z.cx; };
    int checked = 0, cut = 0;
    auto report = [&](HWND c, const std::wstring& t, int need, int have, const char* what) {
        ++cut;
        LOGW("settings: text does not fit: %s id %d '%s' needs %d px, has %d", what, GetDlgCtrlID(c), elog::Narrow(t).c_str(), need, have);
    };
    for (HWND c = GetWindow(g_dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        // shown on its tab (every tab is checked, not only the one on screen); the dialog itself is not shown yet: not IsWindowVisible
        const int tab = TabOf(c);
        if (tab >= 0 ? WantHidden(c) : !(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
        wchar_t cls[32] = L"";
        GetClassNameW(c, cls, 32);
        RECT rc{};
        GetClientRect(c, &rc);
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) continue;
        const LONG style = GetWindowLongW(c, GWL_STYLE);
        wchar_t buf[1024] = L"";
        GetWindowTextW(c, buf, 1024);
        const std::wstring t = buf;
        if (!_wcsicmp(cls, glass::kStatic)) {
            if ((style & SS_TYPEMASK) == SS_OWNERDRAW || (style & SS_ELLIPSISMASK) || t.empty() || GetDlgCtrlID(c) == kFooterBgId) continue;
            ++checked;
            RECT need{0, 0, w, 0};
            DrawTextW(dc, t.c_str(), (int)t.size(), &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            if (need.bottom > h) report(c, t, need.bottom, h, "label (height: wraps to more lines than fit)");
        } else if (!_wcsicmp(cls, glass::kButton)) {
            if (t.empty()) continue;
            ++checked;
            const LONG type = style & BS_TYPEMASK;
            const int tw = textW(t);
            if (type == BS_GROUPBOX) { if (tw + Px(16) > w) report(c, t, tw + Px(16), w, "group box"); }
            else if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX) {
                const int need = tw + Px(glass::kCheckTextX);
                if (need > w) report(c, t, need, w, "check box");
            } else if (tw + Px(12) > w) report(c, t, tw + Px(12), w, "button");
        } else if (!_wcsicmp(cls, glass::kLink)) {
            if (t.empty()) continue;
            ++checked;
            std::wstring plain;   // the text without the <a ...> / </a> tags
            for (size_t i = 0; i < t.size(); ++i) {
                if (t[i] == L'<') { const size_t e = t.find(L'>', i); if (e != std::wstring::npos) { i = e; continue; } }
                plain += t[i];
            }
            const int tw = textW(plain);
            if (tw > w) report(c, plain, tw, w, "link");
        } else if (!_wcsicmp(cls, glass::kCombo)) {
            RECT wr{};
            GetWindowRect(c, &wr);
            const int have = (wr.right - wr.left) - Px(glass::kComboTextPad);   // the closed combo: minus the text inset and the chevron
            const int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0);
            for (int i = 0; i < n; ++i) {
                wchar_t item[256] = L"";
                if (SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)i, 0) >= 255) continue;
                SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)item);
                ++checked;
                const int tw = textW(item);
                if (tw > have) report(c, item, tw, have, "drop-down item");
            }
        }
    }
    // the Description panel: every help text must fit its visible lines without scrolling the edit
    if (HWND he = Ctl(ID_HELPTEXT)) {
        RECT fr{};
        SendMessageW(he, EM_GETRECT, 0, (LPARAM)&fr);
        const int visible = (fr.bottom - fr.top) / Builder{g_dlg}.LineHPx();
        auto checkHelp = [&](const wchar_t* t) {
            ++checked;
            SetWindowTextW(he, t);
            const int lines = (int)SendMessageW(he, EM_GETLINECOUNT, 0, 0);
            if (lines > visible) report(he, std::wstring(t).substr(0, 60), lines, visible, "description (lines)");
        };
        checkHelp(kOverview);
        for (const Help& hp : kHelp) checkHelp(hp.text);
        SetWindowTextW(he, kOverview);
        g_shown = kOverview;
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(g_dlg, dc);
    LOGI("settings: text fit check: %d controls, %d cut", checked, cut);
}

} // namespace

HWND DialogWindow() { return g_dlg; }

void SetQueueHooks(const QueueHooks& hooks) { g_hooks = hooks; }
void SetOpenSteamVrHook(std::function<void()> hook) { g_openSteamVr = std::move(hook); }
void SetToolHooks(const ToolHooks& hooks) { g_tools = hooks; }
void SetUpdateHooks(const UpdateHooks& hooks) { g_update = hooks; }

void NotifyQueueChanged() {
    if (g_dlg) PostMessageW(g_dlg, WM_QUEUE_POKE, 0, 0);
}

void NotifyUpdateChanged() {
    if (g_dlg) PostMessageW(g_dlg, WM_UPDATE_POKE, 0, 0);
}

void ShowDialog(const std::wstring& configPath, std::function<void(const std::vector<std::wstring>&)> onSaved) {
    if (g_dlg) {
        ShowWindow(g_dlg, SW_RESTORE);
        SetForegroundWindow(g_dlg);
        return;
    }
    // Per-monitor DPI-aware while the window lives: it draws at the real size of its screen, so its text is sharp (a
    // DPI-unaware window is drawn at 96 DPI and stretched by Windows: blurry). The rest of the tray stays DPI-unaware; the
    // thread's context goes back at the end. The window opens on the primary screen: its DPI first (checked again below).
    const DPI_AWARENESS_CONTEXT oldDpiCtx = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        UINT dx = 96, dy = 96;
        if (FAILED(GetDpiForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy)) || !oldDpiCtx) dx = 96;
        g_s = dx / 96.0;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);
    glass::Init();
    glass::Reload();   // the theme may have changed while no window was open
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // SHBrowseForFolder
    g_path = configPath;
    g_onSaved = onSaved;
    g_done = false;
    g_capturing = 0;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   // the app icon (tray/tray.rc): title bar, taskbar, Alt+Tab
        wc.hbrBackground = nullptr;   // WM_ERASEBKGND: glass::PaintBackground (Mica or the solid theme colour)
        wc.lpszClassName = L"EyeCaptureSettings";
        RegisterClassW(&wc);
        registered = true;
    }
    g_scrollY = g_scrollMax = g_viewH = 0;
    g_made.clear();
    g_madeTab.clear();
    g_wantHidden.clear();
    g_helpMinH = g_helpPad = g_naturalH = 0;
    RECT rc{0, 0, Px(kLayoutW), Px(700)};   // provisional: resized below to the height BuildControls computed
    // no WS_SYSMENU: DWM would draw its caption buttons over the header (WM_NCCALCSIZE removes the title bar; Minimize / Exit are in the footer)
    const DWORD style = WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
    AdjustFrame(rc, style);
    const int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
    g_dlg = CreateWindowExW(WS_EX_APPWINDOW, L"EyeCaptureSettings", kWindowTitle, style, sx < 0 ? 0 : sx, sy < 0 ? 0 : sy,
                            w, hgt, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_dlg) {
        if (const UINT wd = glass::WindowDpi(g_dlg); wd && wd != Dpi()) g_s = wd / 96.0;
        MakeFonts(Dpi());
        LOGI("settings: window DPI %u (per-monitor aware: %s)", Dpi(), oldDpiCtx ? "yes" : "no");
        glass::ApplyWindow(g_dlg);
        // Create at the narrowest width, measure the help texts there, then one Move pass at the same provisional size: it
        // gives the natural content height with the measured Description panel (g_naturalH; independent of the window size).
        RECT pc{};
        GetClientRect(g_dlg, &pc);
        int natural = 0;
        BuildControls(g_dlg, Builder::Pass::Create, Px(kLayoutW), pc.bottom - pc.top, 0, &natural);
        ApplyTab();
        ApplyGlassTheme();
        MeasureHelp();
        BuildControls(g_dlg, Builder::Pass::Move, Px(kLayoutW), pc.bottom - pc.top, 0, &natural);
        g_naturalH = natural;
        RECT wa{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) wa = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        // the whole content without scrolling when the work area allows it; else as high as the work area (Relayout shows the
        // scroll bar; MinClientW() already leaves room for it)
        RECT fit{0, 0, Px(MinClientW()), Px(g_naturalH + g_footerH)};
        AdjustFrame(fit, style);
        const int fw = fit.right - fit.left, maxH = wa.bottom - wa.top;
        const int fh = fit.bottom - fit.top < maxH ? fit.bottom - fit.top : maxH;
        const int fx = wa.left + (wa.right - wa.left - fw) / 2, fy = wa.top + (maxH - fh) / 2;
        SetWindowPos(g_dlg, nullptr, fx < wa.left ? wa.left : fx, fy < wa.top ? wa.top : fy, fw, fh, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();   // WM_SIZE did it already unless the size did not change
        SetTimer(g_dlg, kHoverTimer, 100, nullptr);
        SetTimer(g_dlg, kPulseTimer, 50, nullptr);
        SetTimer(g_dlg, kQueueTimer, 500, nullptr);
        LoadFromFile();
        g_savedKv = ComparableValues();
        UpdateQueueView();
        UpdateUpdateLine();
        CheckTextFit();
        ShowWindow(g_dlg, SW_SHOW);
        SetForegroundWindow(g_dlg);

        MSG m;
        while (!g_done) {
            const BOOL got = GetMessageW(&m, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage((int)m.wParam);   // the tray is exiting: pass WM_QUIT on
                break;
            }
            if (m.message == WM_NULL) continue;
            if (g_dlg && CaptureKey(m)) continue;
            // Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+PageDown / Ctrl+PageUp: next / previous tab
            if (g_dlg && m.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) &&
                (m.wParam == VK_TAB || m.wParam == VK_NEXT || m.wParam == VK_PRIOR) && (m.hwnd == g_dlg || IsChild(g_dlg, m.hwnd))) {
                const bool back = m.wParam == VK_PRIOR || (m.wParam == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000));
                SetTab(g_tab + (back ? -1 : 1));
                if (HWND s = Ctl(ID_TABS)) InvalidateRect(s, nullptr, FALSE);
                continue;
            }
            if (g_dlg && IsDialogMessageW(g_dlg, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (g_dlg) DestroyWindow(g_dlg);
    }
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_odFont) { DeleteObject(g_odFont); g_odFont = nullptr; }
    if (g_odFontLg) { DeleteObject(g_odFontLg); g_odFontLg = nullptr; }
    if (g_logoFont) { DeleteObject(g_logoFont); g_logoFont = nullptr; }
    if (g_tabFont) { DeleteObject(g_tabFont); g_tabFont = nullptr; }
    if (g_iconFont) { DeleteObject(g_iconFont); g_iconFont = nullptr; }
    g_dlg = nullptr;
    g_onSaved = nullptr;
    if (SUCCEEDED(co)) CoUninitialize();
    if (oldDpiCtx) SetThreadDpiAwarenessContext(oldDpiCtx);
}

} // namespace settings
