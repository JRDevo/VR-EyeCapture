// settings -- see settings.h.
#include "settings.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "input.h"
#include "disk.h"
#include "install.h"
#include "log.h"
#include "postprocess.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace settings {

const wchar_t* const kWindowTitle = L"OpenXR-EyeCapture Settings";

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
    {L"output_dir",        L"capture",         Kind::Edit,  ID_OUTDIR,     nullptr},
    {L"codec",             L"capture",         Kind::Combo, ID_CODEC,      L"hevc|h264|av1",
     L"H.265 / HEVC (recommended)|H.264 / AVC|AV1 (hardware: RTX 40 / 50)"},
    {L"bitrate_mbps",      L"capture",         Kind::Combo, ID_BITRATE,    L"0|150|200|250|300",
     L"Default (120 Mbit/s)|150 Mbit/s|200 Mbit/s|250 Mbit/s|300 Mbit/s", L"Custom (%s Mbit/s)"},
    {L"max_fps",           L"capture",         Kind::Edit,  ID_MAXFPS,     nullptr},
    {L"max_width",         L"capture",         Kind::Combo, ID_MAXWIDTH,   L"0|3840|2560|1920",
     L"Native (full resolution)|3840 px (4K)|2560 px (2K)|1920 px (1080p)", L"Custom (%s px)"},
    {L"require_tray",      L"capture",         Kind::Check, ID_REQTRAY,    nullptr},
    {L"swap_eyes",         L"capture",         Kind::Check, ID_SWAP,       nullptr},
    {L"record_overlays",   L"capture",         Kind::Check, ID_OVERLAYS,   nullptr},
    {L"record_for_youtube", L"capture",        Kind::Check, ID_RECORDYT,   nullptr},
    {L"symmetric_fov",     L"head_smoothing",  Kind::Check, ID_SYMFOV,     nullptr},
    {L"head_smoothing",    L"head_smoothing",  Kind::Track, ID_SMOOTH,     nullptr},
    {L"roll_lock",         L"head_smoothing",  Kind::Check, ID_ROLL,       nullptr},
    {L"output_fov_scale",  L"head_smoothing",  Kind::Edit,  ID_FOVSCALE,   nullptr},
    {L"projection",        L"dome",            Kind::Combo, ID_PROJECTION, L"flat|vr180|vr360", L"Flat stereo (side by side)|VR180 dome (experimental)|VR360 dome (experimental)"},
    {L"layout_3d",         L"dome",            Kind::Combo, ID_LAYOUT3D,   L"tb_half|sbs_half|sbs_full|tb_full",
     L"Top/bottom, half height (YouTube)|Side by side, half width|Side by side, full|Top/bottom, full"},
    {L"fill",              L"dome",            Kind::Combo, ID_FILL,       L"black|last_seen", L"Black|Hold last seen view"},
    {L"fill_fade_s",       L"dome",            Kind::Edit,  ID_FILLFADE,   nullptr},
    {L"vr_indicator",      L"indicator",       Kind::Check, ID_INDICATOR,  nullptr},
    {L"indicator_x",       L"indicator",       Kind::Edit,  ID_INDX,       nullptr},
    {L"indicator_y",       L"indicator",       Kind::Edit,  ID_INDY,       nullptr},
    {L"indicator_z",       L"indicator",       Kind::Edit,  ID_INDZ,       nullptr},
    {L"indicator_size",    L"indicator",       Kind::Edit,  ID_INDSIZE,    nullptr},
    {L"indicator_opacity", L"indicator",       Kind::Edit,  ID_INDOPACITY, nullptr},
    {L"audio_enabled",     L"audio",           Kind::Check, ID_AUDIO,      nullptr},
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
    if (f->kind == Kind::Track || key == L"bitrate_mbps" || key == L"yt8k_bitrate_mbps" || key == L"max_fps" || key == L"max_width" || key == L"output_fov_scale" ||
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
    if (key == L"projection" || key == L"fill" || key == L"layout_3d" || key == L"post_when" || key == L"eye_crop") return Lower(v);
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

Reporter g_reporter = nullptr;
void Report(const std::wstring& title, const std::wstring& text) {
    if (g_reporter) { g_reporter(title, text); return; }
    MessageBoxW(DialogWindow(), text.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
}

} // namespace

void SetReporter(Reporter r) { g_reporter = r; }

KeyValues FromConfig(const Config& c) {
    KeyValues kv;
    auto B = [](bool b) { return std::wstring(b ? L"1" : L"0"); };
    std::wstring hk = c.hotkey.text;
    if (hk.empty()) hk = L"Ctrl+Shift+F10";
    kv.emplace_back(L"hotkey", hk);
    kv.emplace_back(L"output_dir", c.outputDir);
    kv.emplace_back(L"codec", FromUtf8(VideoCodecKey(c.codec)));
    kv.emplace_back(L"bitrate_mbps", std::to_wstring(c.bitrateMbps));
    kv.emplace_back(L"max_fps", std::to_wstring(c.maxFps));
    kv.emplace_back(L"max_width", std::to_wstring(c.maxWidth));
    kv.emplace_back(L"require_tray", B(c.requireTray));
    kv.emplace_back(L"swap_eyes", B(c.swapEyes));
    kv.emplace_back(L"record_overlays", B(c.recordOverlays));
    kv.emplace_back(L"record_for_youtube", B(c.recordForYoutube));
    kv.emplace_back(L"symmetric_fov", B(c.head.symmetricFov));
    kv.emplace_back(L"head_smoothing", FloatText(c.head.smoothing));
    kv.emplace_back(L"roll_lock", B(c.head.rollLock));
    kv.emplace_back(L"output_fov_scale", FloatText(c.head.fovScale));
    kv.emplace_back(L"projection", FromUtf8(DomeProjectionName(c.dome.projection)));
    static const wchar_t* yl[] = {L"tb_half", L"sbs_half", L"sbs_full", L"tb_full"};
    kv.emplace_back(L"layout_3d", yl[(int)c.layout3d >= 0 && (int)c.layout3d < 4 ? (int)c.layout3d : 0]);
    kv.emplace_back(L"fill", c.dome.fill == DomeFill::LastSeen ? L"last_seen" : L"black");
    kv.emplace_back(L"fill_fade_s", FloatText(c.dome.fadeS));
    kv.emplace_back(L"vr_indicator", B(c.indicator.enabled));
    kv.emplace_back(L"indicator_x", FloatText(c.indicator.x));
    kv.emplace_back(L"indicator_y", FloatText(c.indicator.y));
    kv.emplace_back(L"indicator_z", FloatText(c.indicator.z));
    kv.emplace_back(L"indicator_size", FloatText(c.indicator.size));
    kv.emplace_back(L"indicator_opacity", FloatText(c.indicator.opacity));
    kv.emplace_back(L"audio_enabled", B(c.audioEnabled));
    kv.emplace_back(L"audio_device", c.audioDevice);
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
    ParseHotkey(L"Ctrl+Shift+F10", defCfg.hotkey);
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
    return SaveKeyValuesToFile(path, FromConfig(c), r);
}

bool HotkeyText(unsigned vk, bool ctrl, bool shift, bool alt, std::wstring& out) {
    return KeyBindingText(vk, ctrl, shift, alt, out);   // one naming table: src/hotkey.cpp
}

// ================================================================================================ the window
namespace {

HWND                g_dlg = nullptr;
HWND                g_tip = nullptr;            // the tooltip window (owned by the dialog)
HFONT               g_font = nullptr;
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
QueueHooks          g_hooks;                // conversion worker callbacks (SetQueueHooks)
std::vector<unsigned long long> g_qIds;     // job ids of the waiting-list items, same order as the list box
std::vector<std::wstring> g_qItems;         // their texts (the list is only rebuilt when this changes: no flicker)
bool                g_qRunNow = false;      // the Pause button reads "Run now" (the running job waits for the hold)
const UINT          WM_QUEUE_POKE = WM_APP + 20;   // NotifyQueueChanged -> refresh the queue view
const UINT_PTR      kQueueTimer = 5;        // 500 ms: refresh the queue view (progress / ETA) even without a poke
std::vector<HWND>   g_dim;                  // static texts drawn in the grey hint colour
std::vector<HWND>   g_footer;               // footer controls: kept at the bottom of the window while the content scrolls
int                 g_contentH = 0;         // client height of the content above the footer
int                 g_footerH = 0;
int                 g_viewH = 0;            // visible content height when the window is shorter than the content (scrolling)
int                 g_scrollY = 0, g_scrollMax = 0;   // scroll offset / its maximum (0 = nothing to scroll)
std::vector<HWND>   g_made;                 // every control made by Builder::Make, in creation order (the Move pass walks it)
int                 g_helpMinH = 0;         // height of the help edit that shows the longest help text without cut (MeasureHelp)
int                 g_helpPad = 0;          // help edit client height minus its default formatting rectangle height
int                 g_naturalH = 0;         // content height with the help edit at g_helpMinH (independent of the window size)
size_t              g_contentEnd = 0;       // g_made index of the first footer control (set by the Create pass)
bool                g_footerRaised = false; // the footer is above the content in the z-order (only while scrolling)
const int           kLayoutW = 1140;        // the narrowest layout: client width without the scroll bar
const int           kMinClientH = 400;      // minimum client height (the content scrolls below its natural height)
const int           kFooterH = 64;
const int           kFooterBgId = 3001;     // opaque owner-drawn strip behind the footer buttons
// The client is never narrower than kLayoutW, even with the vertical scroll bar shown.
int MinClientW() { return kLayoutW + GetSystemMetrics(SM_CXVSCROLL); }
void Relayout();


// ---- help texts: hover tooltip + the "Description" panel. Product-documentation style: short, neutral, complete sentences.
const wchar_t* const kOverview =
    L"Point at a setting to see its description here.\r\n"
    L"How it works: eye_capture.dll is an OpenXR API layer. While this tray app is running and the hotkey is pressed, the layer copies the "
    L"two eye images that the game submits to the headset, encodes them on the GPU and writes an MP4 file, which the tray then converts "
    L"to a 3D MKV for YouTube (or, with 'Record directly in the YouTube layout' in the Export group, only copies it into the MKV). "
    L"Nothing is injected into the game. Saved changes apply to the next recording.";

struct Help { int id; const wchar_t* text; };
const Help kHelp[] = {
    {ID_HOTKEY, L"Starts and stops a recording while the game is running. Press the hotkey once to start and again to stop. "
                L"Choose a key that the game does not use (ScrollLock works well). Several hotkeys may be listed, separated by commas; any of them works.\r\n"
                L"Keyboard keys and mouse buttons (MouseMiddle, MouseX1, MouseX2) are read when the game starts, so restart the game after changing them.\r\n"
                L"Xbox controller: Pad:A, Pad:Back+Start (hold both), Pad2:LB (second controller only). "
                L"Wheel or joystick: Joy:*:Button3 (any device) or Joy:T300RS:Button5+Button6 (any device whose name contains T300RS); "
                L"POVUp, POVDown, POVLeft and POVRight address the hat switch. "
                L"Controller and wheel buttons take effect immediately.\r\n"
                L"To list the button names of a wheel, run EyeCaptureTray.exe --inputs in a command prompt."},
    {ID_HOTKEY_PICK, L"Click, then press a key, a mouse side button, an Xbox controller button (hold several for a combination) or a "
                     L"wheel or joystick button. Esc cancels. The captured input replaces the current contents of the field."},
    {ID_HOTKEY_ADD, L"Click, then press a key, a mouse side button, an Xbox controller button (hold several for a combination) or a "
                    L"wheel or joystick button. Esc cancels. The captured input is added to the list, so the existing hotkeys keep working."},
    {ID_CODEC, L"Video compression format. HEVC (H.265) gives the best quality per megabyte and is the format YouTube expects for "
               L"high-resolution 3D video. H.264 (AVC) is older and plays in more programs, but produces larger or softer files "
               L"(and is limited to 4096 px wide). AV1 needs a graphics card that encodes AV1 (NVIDIA RTX 40 / 50 series): about "
               L"the HEVC quality at two thirds of the bitrate (default 80 Mbit/s). For 8K at VR frame rates the encoder runs at "
               L"its faster quality setting so no frames are dropped. Without an AV1 encoder the recording is made in HEVC."},
    {ID_BITRATE, L"Video bitrate of the recording in megabits per second. A higher bitrate gives a sharper picture (most visible in fast "
                 L"motion) and larger files. Default: 120 Mbit/s for HEVC, 80 Mbit/s for H.264 and AV1.\r\n"
                 L"File size per hour of recording: 120 Mbit/s = about 54 GB, 150 = 68 GB, 200 = 90 GB, 250 = 113 GB, 300 = 135 GB.\r\n"
                 L"Suggested: 250 Mbit/s at 60 fps or more, the default 120 Mbit/s at 30 fps. A value set in config.ini that is not in "
                 L"the list is shown as Custom and is preserved."},
    {ID_BITRATEHINT, L"File size of one hour of recording at the selected bitrate (video only; audio adds about 0.1 GB)."},
    {ID_YT8KBITRATE, L"Bitrate of the 8K 3D MKV (YouTube output with 8K) when the NVIDIA encoder is used. 150 Mbit/s (default) keeps "
                     L"YouTube's 8K tier well fed; 200 Mbit/s gives a little more detail in fast motion and larger files; 100 Mbit/s "
                     L"makes smaller files. The peak rate is 4/3 of the value. With the AV1 8K codec the same value gives a better "
                     L"picture: about 100 Mbit/s AV1 looks like 150 Mbit/s HEVC. Without an NVIDIA GPU the slower x265 encoder is used "
                     L"at constant quality and this setting does not apply. Unavailable with 'Record directly in the YouTube layout' "
                     L"(no 8K file is made: the recording itself is uploaded)."},
    {ID_YT8KCODEC, L"Encoder of the 8K 3D MKV. HEVC (default) works on every NVIDIA GPU with NVENC. AV1 needs an NVIDIA RTX 40 / 50 "
                   L"series GPU: a sharper picture at the same 8K bitrate (8-bit AV1, which YouTube accepts). Without AV1 support the "
                   L"8K file is made in HEVC. Only used when the YouTube output includes the 8K file. An AV1 8K file is named "
                   L"<name>_YT8K_AV1.mkv (with a crop tag: _YT8K_4x3_AV1.mkv), so an HEVC and an AV1 copy of one recording can sit "
                   L"side by side; when HEVC had to be used, the file gets the plain name. Unavailable with 'Record directly in the "
                   L"YouTube layout' (no 8K file is made)."},
    {ID_OUTFREE, L"Free space on the drive of the output folder. Recordings and their converted files (MKV, 8K, flat) are all written "
                 L"there; nothing large goes to drive C: unless the folder is on C:. A recording or conversion does not start with "
                 L"less than 10 GB free, and a recording stops (and is saved) when the drive drops below 2 GB."},
    {ID_MAXWIDTH, L"Width of the recorded video: the whole side-by-side frame, both eyes together. "
                  L"Native uses the full size that the headset provides (very large on a Quest 3, up to the codec limit).\r\n"
                  L"A smaller size reduces the GPU load during recording (the symmetric view and head smoothing pass is drawn at this size, "
                  L"not at full size) and produces smaller files. 3840 px (4K) is a good balance; use 2560 px (2K) or 1920 px (1080p) on weaker GPUs. "
                  L"The height follows the aspect ratio of the picture. A value set manually in config.ini is shown as Custom and is preserved. "
                  L"Applies to the next recording."},
    {ID_MAXFPS, L"Limits the frame rate of the recording. 0 records every frame that the headset displays (72 or 90 fps). "
                L"60 or 30 fps produces smaller files and a lower GPU load. YouTube plays 60 fps."},
    {ID_OUTDIR, L"Folder in which recordings are saved; the converted files go next to them. If empty, the 'captures' folder next to "
                L"eye_capture.dll is used. Pick a folder on a drive with plenty of free space (about 54 GB per hour at the default bitrate)."},
    {ID_BROWSE, L"Selects the output folder, on any drive. The free space of that drive is shown below the field."},
    {ID_AUDIO, L"Records the system audio (the Windows playback device) into the video."},
    {ID_OVERLAYS, L"Includes the panels that the game or other mods display as separate layers in the headset, such as menus, the DLAA "
                  L"injector's VR panel and its fps box. They are positioned and blended into the video as the headset shows them. "
                  L"When cleared, only the game's 3D view is recorded.\r\n"
                  L"The Virtual Desktop and Quest system menus (and the guardian) are added by the headset itself and cannot be "
                  L"recorded. The red recording indicator is never recorded. Applies to the next recording."},
    {ID_AUDIODEV, L"Part of the name of the playback device to record, for example 'Virtual Desktop Audio'. "
                  L"If empty, the Windows default playback device is used."},
    {ID_PROJECTION, L"Flat stereo produces a regular side-by-side 3D video that shows exactly what was seen in the headset. "
                    L"Recommended; it plays everywhere.\r\n"
                    L"VR180 and VR360 are EXPERIMENTAL. The video becomes a dome that can be explored with a VR player. "
                    L"However, the game draws only what the headset is looking at (about 100 degrees). The dome shows live video there, "
                    L"older video where the view was previously, and black where it was never looked at. It is not a live 360-degree world. "
                    L"Requires a VR180 or VR360 player (YouTube VR, DeoVR, Virtual Desktop Videos). "
                    L"Symmetric field of view and head smoothing are not used in dome mode."},
    {ID_FILL, L"Dome projections (VR180 / VR360) only; unavailable while Projection is Flat stereo. 'Hold last seen view' keeps "
              L"previously viewed areas on the dome until they fade. 'Black' shows only the live view; everything else is black."},
    {ID_FILLFADE, L"Dome projections only; unavailable while Projection is Flat stereo. Time in seconds that an older area remains "
                  L"visible before it fades to black (the fade itself takes 2 seconds). 0 = never fade."},
    {ID_SYMFOV, L"Flat projection only. The headset renders each eye with an asymmetric field of view (wider on the outside). "
                L"This option re-projects both eyes to one symmetric view, so that a 3D player can fuse them without eye strain. "
                L"Costs one GPU pass per frame. Clear it only to record the raw eye images."},
    {ID_SWAP, L"Places the right eye in the left half and the left eye in the right half of the frame. Use it if the 3D effect appears inside out."},
    {ID_ROLL, L"Flat projection with head smoothing above 0. Keeps the horizon level in the video when the head is tilted."},
    {ID_SMOOTH, L"Flat projection only. 0 = off: the video follows the head exactly. Higher values remove small head movements; "
                L"each frame is re-projected against a smoothed head direction. Large turns still follow the head. "
                L"Costs one GPU pass per frame. Black edges are prevented automatically (edge_guard in config.ini, on by default)."},
    {ID_FOVSCALE, L"Flat projection with head smoothing on. The video shows this fraction of the game's field of view (0.5 to 1). "
                  L"The trimmed border is the margin that the smoothing correction needs at the edges. 1 = no margin; "
                  L"0.9 = about 3 degrees on every side (default). Smaller values leave more margin and show less of the view."},
    {ID_INDICATOR, L"Shows a small red dot in the headset while a recording is in progress. The dot is not part of the video. "
                   L"Direct3D 11 games only (ETS2 / ATS). Read when the game starts."},
    {ID_INDX, L"Horizontal position of the dot relative to straight ahead, in metres. 0 = centre, positive = right, negative = left. Default 0.22."},
    {ID_INDY, L"Vertical position of the dot, in metres. 0 = eye level, positive = up, negative = down. Default -0.16 (slightly below centre)."},
    {ID_INDZ, L"Distance of the dot in front of the viewer, in metres. Always negative (-0.8 = 80 cm ahead). A greater distance makes the dot appear smaller. Default -0.8."},
    {ID_INDSIZE, L"Edge length of the dot in metres at its distance Z. At -0.8 m: 0.02 is about 1.4 degrees, 0.005 is a small dot, "
                 L"0.002 is a few pixels. Below about 0.001 the dot is smaller than one headset pixel and disappears. The smallest allowed value is 0.0005."},
    {ID_INDOPACITY, L"Opacity of the red dot. 1 = fully visible, 0.3 = faint."},
    {ID_INDPREVIEW, L"Approximate position of the dot in the headset view. The circle represents the field of view; the centre is straight ahead."},
    {ID_LAYOUTPREVIEW, L"Scale drawing of the files that the tray creates, with their sizes in pixels. Left: the recording (an example "
                       L"from a Quest 3: 8192x4630, both eyes side by side; the hatched part of each eye is removed by Eye crop). "
                       L"Right: the 3D copy, with the left eye (L) and right eye (R) arranged as set in Output layout. It follows Resolution, "
                       L"Codec, Output layout, Eye crop, YouTube output, Record directly in the YouTube layout and Projection. With "
                       L"'Record directly in the YouTube layout', the left side is the headset's view and the right side is the recording "
                       L"itself, which is uploaded as it is after a quick copy into the MKV. VR180 and VR360 recordings are copied unchanged."},
    {ID_YTMODE, L"Files that the tray creates after each recording, for uploading to YouTube.\r\n"
                L"3D MKV: the recording in the selected output layout, with the 3D tags that YouTube reads. With Side by side, full this is a copy that takes "
                L"seconds; the other layouts are re-encoded. YouTube plays it in its 4K tier.\r\n"
                L"8K 3D MKV (higher quality, slower; <name>_YT8K.mkv; with a 4:3 or no eye crop: <name>_YT8K_4x3.mkv / _YT8K_full.mkv; "
                L"with the AV1 8K codec _AV1 is added): 3D in the selected Output layout "
                L"(default top/bottom, half height, short side 4320), 60 fps. "
                L"YouTube reads 3D as half-format, so the picture is not stretched. "
                L"YouTube then keeps its 8K tier and a much higher bitrate, so the 3D picture is much sharper in the headset. "
                L"Re-encoded on the GPU in about twice the clip length. Flat 3D recordings only.\r\n"
                L"A recording made with 'Record directly in the YouTube layout' is already in this form: its 3D MKV is a quick copy and "
                L"no separate 8K file is made.\r\n"
                L"Upload the .mkv file named in the notification. An existing file is never overwritten (the new file gets \" (2)\" in its name)."},
    {ID_RECORDYT, L"Records the video directly in the layout that YouTube needs, so nothing has to be re-encoded afterwards. Each frame is "
                  L"written in the selected Output layout (default top/bottom, half height) with Eye crop applied and a short side of 4320 "
                  L"pixels (a Quest 3 at 16:9 gives 7680x4320). The tray then only copies the recording into the 3D MKV, which takes "
                  L"seconds; that MKV is the file to upload.\r\n"
                  L"The full side-by-side picture is not kept: Output layout and Eye crop are part of the recording and cannot be changed "
                  L"later, and the 8K 3D MKV, 8K bitrate and 8K codec do not apply (the recording already is the 8K file; a 2D video can "
                  L"still be made). Frames wider than 4096 px need HEVC or AV1 (H.264 is limited to 4096 px, so the frame is made smaller); "
                  L"Resolution limits the frame width. A Frame rate limit of 60 fps is recommended (YouTube plays at most 60 fps). "
                  L"Flat recordings only; VR180 and VR360 ignore it. Applies to the next recording."},
    {ID_LAYOUT3D, L"Arrangement of the two eyes in the files that the tray creates from a flat recording (the 3D MKV and the 8K 3D MKV). "
                  L"The recording itself remains full side by side, unless 'Record directly in the YouTube layout' is selected: then the "
                  L"recording is made in this layout. Unavailable for VR180 and VR360, which have fixed layouts.\r\n"
                  L"Top/bottom, half height: the layout that YouTube requires. The frame has the shape of one eye; each eye is squeezed to half height, "
                  L"left eye on top. The MKV is re-encoded (slower than a plain copy).\r\n"
                  L"Side by side, half width: each eye is squeezed to half width. The MKV is re-encoded.\r\n"
                  L"Side by side, full: the recording as it is, left eye on the left, each eye at its own shape. The MKV is a fast copy "
                  L"without re-encoding. YouTube shows it stretched; use it for headset players and 3D TVs.\r\n"
                  L"Top/bottom, full: each eye keeps its own shape, left eye on top. The MKV is re-encoded."},
    {ID_EYECROP, L"Flat recordings often have eyes that are taller than wide. A tall top/bottom video crashes the YouTube player on "
                 L"Quest, and YouTube limits it to 2160p. This setting removes the top and bottom of each eye (the same amount for both, centred) before "
                 L"the 3D MKV and 8K 3D MKV are created; nothing is stretched.\r\n"
                 L"16:9 (default, recommended for YouTube 8K): wide view. The 8K copy is 7680x4320, which plays on Quest and receives "
                 L"YouTube's 8K tier.\r\n"
                 L"4:3: taller view; the 8K copy is 5760x4320. At 5760 px wide YouTube likely puts it in its 4K tier (lower bitrate), "
                 L"so it looks softer than 16:9 on YouTube.\r\n"
                 L"4:3, padded to 16:9: keeps the taller 4:3 view and adds black bars left and right, so the 8K copy is 7680x4320 "
                 L"(the picture 5760 px wide in the middle) and receives YouTube's 8K tier. Nothing is stretched.\r\n"
                 L"None: the full eye is kept, so the video is tall. The Quest YouTube player may crash on it and YouTube stays at 2160p.\r\n"
                 L"An eye that is already wider than the selected shape is not cropped. Flat 3D recordings only; the MKV is re-encoded "
                 L"(not a plain copy) whenever it is cropped. With 'Record directly in the YouTube layout' the crop is applied while "
                 L"recording instead.\r\n"
                 L"With 4:3, 4:3 padded or None, the file names carry a tag (_4x3 / _4x3pad / _full, for example <name>_YT8K_4x3.mkv), so converting one "
                 L"recording with two crop settings keeps both files."},
    {ID_KEEPMP4, L"Keeps the original MP4 next to the MKV files. When cleared, the MP4 is deleted once the 3D MKV has been created and verified. "
                 L"The MP4 is always kept when no 3D MKV is created, and while another conversion of it still waits in the list."},
    {ID_POSTWHEN, L"When the tray converts a finished recording (the MKV, 8K and flat files). Conversion uses the CPU and GPU, "
                  L"so running it during a game can reduce the frame rate.\r\n"
                  L"Immediately: as soon as the recording is saved.\r\n"
                  L"When the VR session ends: once no game has a VR session open (for example, after leaving VR or closing the game). "
                  L"If VR starts again while a conversion is running, the conversion is paused and continues afterwards.\r\n"
                  L"When the game exits (default): once the game that made the recording has exited and no VR game is running.\r\n"
                  L"When I say: never converts on its own. Use 'Convert a recording...' in Settings or the tray menu. Finished recordings "
                  L"wait in the Conversions list below; select them and click Start, or click Start all.\r\n"
                  L"\"Convert last recording now\" and \"Convert a recording...\" always start immediately; recordings added with "
                  L"\"Add to queue...\" always wait for Start."},
    {ID_CONVERTPICK, L"Opens a file dialog in the captures folder. Each MP4 you pick is converted next (after the running conversion), "
                     L"whatever the Convert setting says, with the Export settings shown in this window now (YouTube output, 8K bitrate "
                     L"and codec, eye crop, output layout, 2D video, keep MP4), even when they are not saved. Select several files with "
                     L"Ctrl or Shift; they convert one after another."},
    {ID_ADDQUEUE, L"Opens a file dialog in the captures folder. Each MP4 you pick is added to the Conversions list with the Export "
                  L"settings shown in this window now (saved or not), and WAITS there until you click Start or Start all, whatever "
                  L"the Convert setting says. To make several versions of one recording (for example 16:9 and 4:3, or HEVC and AV1), "
                  L"change the Export settings and add the same file again. Each version gets its own file name (_4x3, _full, _AV1, "
                  L"or \" (2)\")."},
    {ID_QSTATUS, L"The conversion that is running now: file, its format (the files it makes, crop, layout), step (MKV, flat or 8K), "
                 L"progress and the estimated time left. "
                 L"A conversion that waits for the Convert setting (for example, while the game is still running) says why it waits."},
    {ID_QPROGRESS, L"Progress of the running step (needs ffprobe, which ships next to ffmpeg). Yellow while paused."},
    {ID_QPAUSE, L"Pause suspends the running conversion (ffmpeg) without losing progress; Resume continues it. When the conversion "
                L"waits because of the Convert setting, the button reads Run now and starts it at once."},
    {ID_QCANCEL, L"Stops the running conversion and deletes its unfinished output. The MP4 is kept, so it can be converted later. "
                 L"Waiting recordings continue."},
    {ID_QLIST, L"Conversions waiting, in the order they run: file, format (MKV / 8K codec and bitrate / 2D, eye crop, layout) and "
               L"state. \"waiting for you\": added with Add to queue (or restored after the tray restarted); it waits for Start. "
               L"\"waiting: converts when ...\": it starts when the Convert setting allows. Automatic recordings show \"(current "
               L"settings)\": they use the saved settings as they are when the conversion starts. Select several with Ctrl or Shift. "
               L"The list is kept when the tray app is closed (queue.json next to it)."},
    {ID_QSTART, L"Starts the selected conversions, one after another, whatever the Convert setting says."},
    {ID_QSTARTALL, L"Starts every waiting conversion, one after another in list order, whatever the Convert setting says."},
    {ID_QREMOVE, L"Removes the selected conversions from the list. The MP4 is not deleted; convert it later with 'Convert a recording...'."},
    {ID_POSTFLAT, L"Also creates a regular 2D video from the left eye (<name>_FLAT.mp4) for viewers without a headset. "
                  L"The video is re-encoded with ffmpeg and takes a while."},
    {ID_LOGLEVEL, L"Amount of detail written to logs\\eye_capture.log and logs\\tray.log. Off writes nothing. Information is the normal level. "
                  L"Debug is intended for bug reports."},
    {ID_AUTOSTART, L"Starts this tray app when you sign in to Windows, so that it is already running before a VR game starts "
                   L"(the layer only records while the tray is running). Off by default. Writes a 'Run' entry for the current Windows "
                   L"account and its 'StartupApproved' record (the on / off switch Task Manager shows), nothing else; turning it off "
                   L"removes both. Clear this option before deleting the folder."},
    {ID_OPENINI, L"Opens config.ini in a text editor. The file lists every setting, with comments."},
    {ID_DEFAULTS, L"Resets every field to its built-in default. Nothing is saved until Save is clicked."},
    {ID_SAVE, L"Writes the changed values to config.ini. The next recording uses them."},
    {ID_CANCEL, L"Closes the window without saving."},
};

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

// "Make for YouTube" drop-down <-> the two hidden checks bound to post_mkv / post_yt8k.
// 0 = 3D .mkv (mkv 1, 8k 0), 1 = 8K 3D .mkv (0, 1), 2 = both files (1, 1), 3 = nothing (0, 0)
HWND g_layoutLabel = nullptr;   // label of the 3D layout drop-down (greyed together with it)
std::vector<HWND> g_domeLabels;  // "Dome fill:", "Fade after:", "s": greyed with the dome fields (UpdateLayoutEnabled)
// The 3D layout and "Record directly in the YouTube layout" only apply to flat recordings: VR180 / VR360 (projection items 1
// and 2) have fixed layouts. The other way round, Dome fill / Fade after only apply to VR180 / VR360. Called on CBN_SELCHANGE of
// ID_PROJECTION and from SetControls.
void UpdateLayoutEnabled() {
    const bool flat = (int)SendMessageW(Ctl(ID_PROJECTION), CB_GETCURSEL, 0, 0) <= 0;
    EnableWindow(Ctl(ID_LAYOUT3D), flat);
    if (g_layoutLabel) EnableWindow(g_layoutLabel, flat);
    EnableWindow(Ctl(ID_RECORDYT), flat);
    EnableWindow(Ctl(ID_FILL), !flat);
    EnableWindow(Ctl(ID_FILLFADE), !flat);
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
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const double kPi = 3.14159265358979323846;
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const bool on = g_prev.on;
    const COLORREF bg = on ? RGB(24, 26, 32) : RGB(52, 52, 52), edge = on ? RGB(120, 128, 150) : RGB(95, 95, 95);
    const COLORREF cross = on ? RGB(52, 56, 68) : RGB(66, 66, 66), txt = on ? RGB(150, 156, 172) : RGB(120, 120, 120);
    HBRUSH bgBrush = CreateSolidBrush(bg);
    const RECT all{0, 0, w, h};
    FillRect(dc, &all, bgBrush);
    DeleteObject(bgBrush);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = g_font ? SelectObject(dc, g_font) : nullptr;

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
        const COLORREF col = on ? Mix(bg, RGB(235, 40, 40), a) : RGB(110, 110, 110);
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
    BitBlt(di->hDC, rc.left, rc.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
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
    if (def) dir = ExeFolder() + L"\\captures";
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

void UpdateLayoutPreview() {
    if (g_dlg && Ctl(ID_LAYOUTPREVIEW)) InvalidateRect(Ctl(ID_LAYOUTPREVIEW), nullptr, FALSE);
}

// ---- the logo at the top of the window (ID_LOGO, owner-drawn): the mark of scripts\make_logo.py (VR goggles, a 256-unit design box),
// the wordmark in bold and the tagline in grey. GDI has no anti-aliasing: everything is drawn SS times bigger into a memory bitmap
// and StretchBlt'ed down with HALFTONE. The layout (logoW) in BuildControls must match the x positions used here.
void DrawLogo(const DRAWITEMSTRUCT* di) {
    const int SS = 4;
    const RECT rc = di->rcItem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    const int W = w * SS, H = h * SS;
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const RECT all{0, 0, W, H};
    FillRect(dc, &all, GetSysColorBrush(COLOR_BTNFACE));

    const double s = (double)H / 256.0;   // the mark is the square (0, 0, H, H)
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

    // wordmark (bold) and tagline (grey), vertically centred on the lens row
    SetBkMode(dc, TRANSPARENT);
    const UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    int x = (32 + 6) * SS;
    auto line = [&](HFONT base, const wchar_t* t, COLORREF c) {
        LOGFONTW lf{};
        if (!base || !GetObjectW(base, sizeof(lf), &lf)) return;
        lf.lfHeight *= SS;
        HFONT f = CreateFontIndirectW(&lf);
        HGDIOBJ of = SelectObject(dc, f);
        SetTextColor(dc, c);
        SIZE sz{};
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        RECT r{x, 0, x + sz.cx + SS, H};
        DrawTextW(dc, t, -1, &r, fmt);
        x += sz.cx;
        SelectObject(dc, of);
        DeleteObject(f);
    };
    line(g_logoFont, L"OpenXR-EyeCapture", RGB(14, 16, 23));
    x += 16 * SS;
    line(g_font, L"Stereo 3D recorder for any OpenXR game", GetSysColor(COLOR_GRAYTEXT));

    SetStretchBltMode(di->hDC, HALFTONE);
    SetBrushOrgEx(di->hDC, 0, 0, nullptr);
    StretchBlt(di->hDC, rc.left, rc.top, w, h, dc, 0, 0, W, H, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void DrawLayoutPreview(const DRAWITEMSTRUCT* di) {
    const RECT rc = di->rcItem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const COLORREF bg = RGB(24, 26, 32), edge = RGB(150, 158, 180), txt = RGB(200, 206, 220), dim = RGB(140, 146, 162);
    const COLORREF colL = RGB(56, 88, 140), colR = RGB(140, 66, 66);
    const RECT all{0, 0, w, h};
    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(dc, &all, bgBrush);
    DeleteObject(bgBrush);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = g_font ? SelectObject(dc, g_font) : nullptr;

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
    BitBlt(di->hDC, rc.left, rc.top, w, h, dc, 0, 0, SRCCOPY);
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
    if (c.hotkey.text.empty()) ParseHotkey(L"Ctrl+Shift+F10", c.hotkey);
    SetControls(FromConfig(c));
    SendMessageW(Ctl(ID_AUTOSTART), BM_SETCHECK, install::AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!problems.empty()) {
        std::wstring m = L"The configuration file contains invalid values on the following lines. The window shows the default value for each of them, and Save replaces them:\n\n";
        for (const auto& p : problems) m += p + L"\n";
        Report(L"Settings", m);
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

void UpdateQueueView() {
    if (!g_dlg) return;
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
                Report(L"Settings", L"The Export settings create no files (YouTube output None and no 2D video). Choose a YouTube output first.");
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

void DoSave() {
    SaveResult r;
    if (!SaveKeyValuesToFile(g_path, CollectControls(), r)) {
        std::wstring m = L"The settings could not be saved:\n\n";
        for (const auto& p : r.problems) m += p + L"\n";
        Report(L"Settings - Not Saved", m);
        return;
    }
    // "Start with Windows" is a registry state, not a config.ini key: apply it after the config was written
    const bool wantAuto = SendMessageW(Ctl(ID_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool hadAuto = install::AutostartEnabled();
    if (wantAuto != hadAuto) {
        std::wstring err;
        if (install::SetAutostart(wantAuto, ExeFolder(), err))
            r.changed.push_back(wantAuto ? L"start_with_windows 0->1" : L"start_with_windows 1->0");
        else
            Report(L"Settings", L"The Start with Windows setting could not be changed:\n" + err);
    }
    if (g_onSaved) g_onSaved(r.changed);
    DestroyWindow(g_dlg);
}

// "Browse...": the modern folder picker (IFileOpenDialog + FOS_PICKFOLDERS; any drive), opened at the current folder (or the
// default captures folder). The free-space line is updated at once.
void DoBrowse() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        Report(L"Settings", L"The folder picker could not be opened. Type the folder into the field instead.");
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Select the folder for recordings (and the converted files)");
    std::wstring start = Trim(GetText(ID_OUTDIR));
    if (start.empty()) start = ExeFolder() + L"\\captures";
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
// Keyboard focus moved to `c`: scroll it into view.
void EnsureVisible(HWND c) {
    if (g_scrollMax <= 0 || !c || !g_dlg) return;
    RECT r;
    GetWindowRect(c, &r);
    MapWindowPoints(nullptr, g_dlg, (POINT*)&r, 2);
    if (r.top < 0) ScrollContent(g_scrollY + r.top - 8);
    else if (r.bottom > g_viewH) ScrollContent(g_scrollY + r.bottom - g_viewH + 8);
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
    if ((id == ID_YTMODE || id == ID_PROJECTION) && code == CBN_SELCHANGE) UpdateYt8kEnabled();
    if (id == ID_OUTDIR && code == EN_CHANGE) SetTimer(g_dlg, kFreeSpaceTimer, kFreeSpaceDelayMs, nullptr);   // debounced: not per key
    if ((id == ID_PROJECTION || id == ID_YTMODE || id == ID_LAYOUT3D || id == ID_EYECROP || id == ID_MAXWIDTH || id == ID_CODEC) &&
        code == CBN_SELCHANGE)
        UpdateLayoutPreview();
    if ((id == ID_PROJECTION || id == ID_YTMODE || id == ID_LAYOUT3D || id == ID_POSTWHEN || id == ID_EYECROP ||
         id == ID_MAXWIDTH || id == ID_BITRATE || id == ID_YT8KBITRATE || id == ID_YT8KCODEC) && code == CBN_SELCHANGE) {
        ShowHelp(HelpText(id));
        return;
    }
    if ((id == ID_SAVE || id == ID_CANCEL || id == ID_OPENINI || id == ID_DEFAULTS || id == ID_BROWSE) && code != BN_CLICKED) return;
    switch (id) {
    case ID_SAVE: DoSave(); break;
    case ID_CANCEL: DestroyWindow(g_dlg); break;
    case ID_OPENINI: ShellExecuteW(nullptr, L"open", g_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_DEFAULTS: {
        Config d;
        ParseHotkey(L"Ctrl+Shift+F10", d.hotkey);
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
    case WM_MOUSEWHEEL:
        if (g_scrollMax > 0) { ScrollContent(g_scrollY - (int)GET_WHEEL_DELTA_WPARAM(wp) * 30 / WHEEL_DELTA); return 0; }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_DRAWITEM:
        if (wp == kFooterBgId) {
            const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
            FillRect(di->hDC, &di->rcItem, GetSysColorBrush(COLOR_BTNFACE));
            RECT line = di->rcItem;
            line.bottom = line.top + 2;
            DrawEdge(di->hDC, &line, EDGE_ETCHED, BF_TOP);
            return TRUE;
        }
        if (wp == ID_INDPREVIEW) { DrawPreview((const DRAWITEMSTRUCT*)lp); return TRUE; }
        if (wp == ID_LAYOUTPREVIEW) { DrawLayoutPreview((const DRAWITEMSTRUCT*)lp); return TRUE; }
        if (wp == ID_LOGO) { DrawLogo((const DRAWITEMSTRUCT*)lp); return TRUE; }
        return DefWindowProcW(h, msg, wp, lp);
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
            if (!IsIconic(h)) UpdateQueueView();
            return 0;
        }
        if (wp == kFreeSpaceTimer) {   // one-shot: UpdateFreeSpace kills it
            UpdateFreeSpace();
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
    case WM_CTLCOLORSTATIC:
        for (HWND d : g_dim)
            if (d == (HWND)lp) {   // hint / caption: grey text on the dialog background
                SetBkMode((HDC)wp, TRANSPARENT);
                SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
        if ((HWND)lp == Ctl(ID_HELPTEXT)) {   // the read-only help edit: same background as the dialog
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_QUEUE_POKE:
        UpdateQueueView();
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Relayout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        RECT r{0, 0, MinClientW(), kMinClientH};
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
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
        g_domeLabels.clear();
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
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int hgt, int id, DWORD ex = 0) {
        if (pass == Pass::Move) {
            if (step >= g_made.size()) { ++step; return nullptr; }   // a sequence mismatch: BuildControls logs it
            HWND c = g_made[step++];
            if (dwp) dwp = DeferWindowPos(dwp, c, nullptr, x, y + dy, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
            if (!dwp) SetWindowPos(c, nullptr, x, y + dy, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
            return c;
        }
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, x, y + dy, w, hgt, parent,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        if (g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_made.push_back(c);
        return c;
    }
    void Tip(HWND c, const wchar_t* t) { if (pass == Pass::Create) AddTip(c, t); }
    // Width of `t` in the dialog font (no padding).
    int TextW(const wchar_t* t, HFONT font = g_font) {
        HDC dc = GetDC(parent);
        HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
        SIZE s{};
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &s);
        if (old) SelectObject(dc, old);
        ReleaseDC(parent, dc);
        return s.cx;
    }
    // A row label (text height 16, centred on a 23 px control that starts at y). helpId != 0: hovering the label shows the help of
    // that control (the label keeps the id in GWLP_USERDATA).
    HWND Label(int x, int y, int w, const wchar_t* t, int helpId = 0) {
        HWND c = Make(L"STATIC", t, SS_LEFT | SS_NOPREFIX | (helpId ? SS_NOTIFY : 0), x, y + 4, w, 16, -1);
        if (helpId) {
            SetWindowLongPtrW(c, GWLP_USERDATA, helpId);
            Tip(c, HelpText(helpId));
        }
        return c;
    }
    // A hint / caption: a label drawn in the grey (COLOR_GRAYTEXT) colour, see WM_CTLCOLORSTATIC.
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
    // Height of one text line in the dialog font (what a multi-line edit / static advances per line).
    int LineH() {
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
        HWND c = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
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
        HWND c = Make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 160, id);
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
        const int dw = widest + GetSystemMetrics(SM_CXVSCROLL) + 12;
        SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)(dw > w ? dw : w), 0);
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
    Builder b{dlg, pass};
    auto keep = [&](std::vector<HWND>& v, HWND c) { if (create) v.push_back(c); return c; };
    g_footerH = kFooterH;
    if (create) {
        g_made.clear();
        g_dim.clear();
        g_footer.clear();
        g_domeLabels.clear();
        g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (g_tip) {
            SetWindowPos(g_tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 380);
            SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(30000, 0));
        }
    } else {
        b.dwp = BeginDeferWindowPos((int)g_made.size());
    }
    b.dy = -scrollY;
    // Two columns of group boxes (left: Recording, Video, Stereo 3D; right: Recording Indicator, Export, Advanced), then the
    // Description and the footer across the full width. setCol() moves the grid to a column: label column at lx, controls at cx.
    const int margin = 12, colGap = 12, fullW = clientW - 2 * margin, colW = (fullW - colGap) / 2;
    const int colWMin = (kLayoutW - 2 * margin - colGap) / 2;
    const int extraW = colW > colWMin ? colW - colWMin : 0;   // the previews grow with the column
    int gx = margin, gw = colW, lx = 0, cx = 0, cw = 0;
    const int labelW = 108;
    auto setCol = [&](int x, int w) { gx = x; gw = w; lx = gx + 12; cx = gx + 132; cw = gx + gw - 12 - cx; };
    setCol(margin, colW);
    const int pitch = 30, top = 22, gap = 8;
    auto groupH = [&](int rows) { return top + rows * pitch + 2; };
    const int unitGap = 6;
    // column heights (rows): left Recording 7 + Video 4 + Stereo 3D 5; right Recording Indicator 5 + Export + Advanced 1. The
    // shorter column's groups get the difference (left: leftEx on Stereo 3D; right: spare, below).
    const int kExportRows = 10;
    const int leftH = groupH(7) + gap + groupH(4) + gap + groupH(5) + gap;
    const int rightH = groupH(5) + gap + groupH(kExportRows) + gap + groupH(1) + gap;
    const int leftEx = rightH > leftH ? rightH - leftH : 0;
    int y = 6;

    // ---- header: the logo (mark + wordmark + tagline, owner-drawn by DrawLogo) and, to its right, the config file path
    {
        const int logoW = 32 + 6 + b.TextW(L"OpenXR-EyeCapture", g_logoFont) + 16 + b.TextW(L"Stereo 3D recorder for any OpenXR game") + 2;
        b.Make(L"STATIC", L"", SS_OWNERDRAW, gx, y, logoW, 32, ID_LOGO);
        const wchar_t* t = L"Configuration file:";
        const int tw = b.TextW(t) + 6;
        const int hx = gx + logoW + 24, hy = y + (32 - 16) / 2;
        b.Make(L"STATIC", t, SS_LEFT | SS_NOPREFIX, hx, hy, tw, 16, -1);
        keep(g_dim, b.Make(L"STATIC", g_path.c_str(), SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, hx + tw, hy, fullW - (logoW + 24 + tw), 16, -1));
    }
    y += 32 + 4;
    const int yTop = y;

    // ---- Recording
    b.Group(gx, y, gw, groupH(7), L"Recording");
    int r = y + top;
    b.Label(lx, r, labelW, L"Hotkey:", ID_HOTKEY);
    b.Edit(ID_HOTKEY, cx, r, cw);
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
    b.Check(ID_AUDIO, cx, r, L"Record system audio");
    r += pitch;
    b.Label(lx, r, labelW, L"Audio device:", ID_AUDIODEV);
    b.Edit(ID_AUDIODEV, cx, r, 190);
    b.Hint(cx + 190 + 8, r, cw - 198, L"Empty = default playback device", ID_AUDIODEV);
    r += pitch;
    b.Label(lx, r, labelW, L"Overlays:", ID_OVERLAYS);
    b.Check(ID_OVERLAYS, cx, r, L"Include overlay layers (menus, mod panels)");
    y += groupH(7) + gap;

    // ---- Video
    b.Group(gx, y, gw, groupH(4), L"Video");
    r = y + top;
    b.Label(lx, r, labelW, L"Codec:", ID_CODEC);
    b.Combo(ID_CODEC, cx, r, 240);
    r += pitch;
    b.Label(lx, r, labelW, L"Bitrate:", ID_BITRATE);
    b.Combo(ID_BITRATE, cx, r, 190);
    SetWindowLongPtrW(b.Hint(cx + 190 + 8, r, cw - 198, L"", ID_BITRATEHINT), GWLP_ID, ID_BITRATEHINT);   // updated by UpdateBitrateHint
    r += pitch;
    b.Label(lx, r, labelW, L"Resolution:", ID_MAXWIDTH);
    b.Combo(ID_MAXWIDTH, cx, r, 190);
    b.Hint(cx + 190 + 8, r, cw - 198, L"Width of the side-by-side frame", ID_MAXWIDTH);
    r += pitch;
    b.Label(lx, r, labelW, L"Frame rate limit:", ID_MAXFPS);
    b.Edit(ID_MAXFPS, cx, r, 60);
    b.Label(cx + 60 + unitGap, r, 24, L"fps", ID_MAXFPS);
    b.Hint(cx + 60 + unitGap + 30, r, 200, L"0 = unlimited", ID_MAXFPS);
    y += groupH(4) + gap;

    // ---- Stereo 3D (Output layout lives in the Export group: it only shapes the files made for YouTube, or the recording
    // itself with record_for_youtube). Dome fill / Fade after are greyed unless Projection is VR180 / VR360.
    b.Group(gx, y, gw, groupH(5) + leftEx, L"Stereo 3D");
    r = y + top;
    b.Label(lx, r, labelW, L"Projection:", ID_PROJECTION);
    b.Combo(ID_PROJECTION, cx, r, 240);
    r += pitch;
    keep(g_domeLabels, b.Label(lx, r, labelW, L"Dome fill:", ID_FILL));
    b.Combo(ID_FILL, cx, r, 160);
    {
        const int fx = cx + 160 + 16, fw = b.TextW(L"Fade after:") + 8;
        keep(g_domeLabels, b.Label(fx, r, fw, L"Fade after:", ID_FILLFADE));
        b.Edit(ID_FILLFADE, fx + fw + 2, r, 50);
        keep(g_domeLabels, b.Label(fx + fw + 2 + 50 + unitGap, r, 14, L"s", ID_FILLFADE));
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
    HWND tb = b.Make(L"msctls_trackbar32", L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, cx, r - 3, 340, 28, ID_SMOOTH);
    b.Tip(tb, HelpText(ID_SMOOTH));
    if (create) {
        SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(0, 20));
        SendMessageW(tb, TBM_SETTICFREQ, 2, 0);
        SendMessageW(tb, TBM_SETPAGESIZE, 0, 2);
        SendMessageW(tb, TBM_SETLINESIZE, 0, 1);
    }
    b.Make(L"STATIC", L"0.00", SS_LEFT | SS_NOPREFIX, cx + 348, r + 4, 44, 16, ID_SMOOTHVAL);
    r += pitch;
    b.Label(lx, r, labelW, L"Field of view scale:", ID_FOVSCALE);
    b.Edit(ID_FOVSCALE, cx, r, 60);
    b.Hint(cx + 60 + 8, r, 120, L"0.5 – 1.0", ID_FOVSCALE);
    y += groupH(5) + leftEx + gap;
    const int yLeftEnd = y;
    y = yTop;
    setCol(margin + colW + colGap, colW);
    // when the right column is shorter, the spare height is shared by its three groups (rows stay on top, previews are
    // centred); when it is longer, the Stereo 3D group takes the difference (leftEx). Today both are 16 rows (576 px): 0 / 0.
    const int spare = yLeftEnd - (yTop + rightH);
    const int ex1 = spare > 0 ? spare / 3 : 0, ex2 = ex1, ex3 = spare > 0 ? spare - 2 * ex1 : 0;

    // ---- Recording Indicator (fields on the left, the live preview of the view on the right; square, it grows with the
    // column up to the inside of the group)
    int prevGrow = groupH(5) + ex1 - top - 10 - 140;
    prevGrow = prevGrow < 0 ? 0 : (extraW < prevGrow ? extraW : prevGrow);
    const int prevSize = 140 + prevGrow, prevY = ex1 > prevGrow ? (ex1 - prevGrow) / 2 : 0;
    b.Group(gx, y, gw, groupH(5) + ex1, L"Recording Indicator");
    r = y + top;
    b.Label(lx, r, labelW, L"Indicator:", ID_INDICATOR);
    b.Check(ID_INDICATOR, cx, r, L"Show recording indicator in headset");
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
    b.Hint(lx, r, 370, L"Z is negative: the indicator floats in front of the viewer.");
    b.Tip(b.Make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, gx + gw - 12 - prevSize, y + top + prevY, prevSize, prevSize, ID_INDPREVIEW),
          HelpText(ID_INDPREVIEW));
    y += groupH(5) + ex1 + gap;

    // ---- Export (fields on the left, the live layout preview on the right). The preview is 213 px wide at the minimum width and
    // grows with the column by up to 40 px (its text lines need that room; DrawLayoutPreview draws at fixed pixel positions, so
    // more would only add empty space); it stays right-aligned in the column and the combos left of it (`ecw`) take the rest.
    const int lpW = 213 + (extraW < 40 ? extraW : 40), lpH = 140;
    const int ecw = gx + gw - 12 - lpW - 12 - cx;   // combo width that leaves a gap before the preview
    // rows: YouTube output, Convert, 8K bitrate, 8K codec, the 2D check (left of the preview: their texts fit `ecw`), Keep MP4,
    // Output layout, Eye crop (below the preview: the layout items are wider than `ecw`), the record_for_youtube check, the two
    // conversion buttons
    b.Group(gx, y, gw, groupH(kExportRows) + ex2, L"Export");
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
    {   // below the preview: the whole control column (Convert = add and start now; Add to queue = add and wait for Start)
        const int bw = (cw - 8) / 2;
        b.Button(ID_CONVERTPICK, cx, r, bw, L"Convert a recording...");
        b.Button(ID_ADDQUEUE, cx + bw + 8, r, cw - bw - 8, L"Add to queue...");
    }
    b.HiddenCheck(ID_POSTMKV);     // bound to post_mkv / post_yt8k, set from / read into the drop-down
    b.HiddenCheck(ID_POSTYT8K);
    b.Tip(b.Make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, gx + gw - 12 - lpW, y + top + ex2 / 2, lpW, lpH, ID_LAYOUTPREVIEW),
          HelpText(ID_LAYOUTPREVIEW));
    y += groupH(kExportRows) + ex2 + gap;

    // ---- Advanced
    b.Group(gx, y, gw, groupH(1) + ex3, L"Advanced");
    r = y + top;
    b.HiddenCheck(ID_REQTRAY);   // require_tray stays a config.ini key (default 1); the tray is required anyway, so no checkbox
    b.Label(lx, r, labelW, L"Log level:", ID_LOGLEVEL);
    b.Combo(ID_LOGLEVEL, cx, r, 160);
    {   // one row: "Startup:" + its check right of the Log level drop-down
        const int sx = cx + 160 + 16, sw = b.TextW(L"Startup:") + 8;
        b.Label(sx, r, sw, L"Startup:", ID_AUTOSTART);
        b.Check(ID_AUTOSTART, sx + sw, r, L"Start with Windows");
    }
    y += groupH(1) + ex3 + gap;
    y = yLeftEnd > y ? yLeftEnd : y;

    // ---- Conversions (full width; the same two-column grid as the groups above): left = the running job (three status lines:
    // file, format, step / progress / time left; progress bar; Pause / Cancel), right = the waiting list (multi-select,
    // scrolls sideways for long rows) with Start / Start all / Remove. Filled by UpdateQueueView.
    {
        const int lh = b.LineH();
        const int stH = 3 * lh, pbY = stH + 6, btY = pbY + 14 + 8, listH = btY + 26;
        const int qH = top + listH + 10;
        setCol(margin, fullW);
        b.Group(gx, y, gw, qH, L"Conversions");
        r = y + top;
        setCol(margin, colW);
        b.Label(lx, r, labelW, L"Running:", ID_QSTATUS);
        b.Tip(b.Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_NOTIFY, cx, r + 2, cw, stH, ID_QSTATUS), HelpText(ID_QSTATUS));
        HWND pb = b.Make(PROGRESS_CLASSW, L"", PBS_SMOOTH, cx, r + 2 + pbY, cw, 14, ID_QPROGRESS);
        if (create) SendMessageW(pb, PBM_SETRANGE32, 0, 100);
        b.Tip(pb, HelpText(ID_QPROGRESS));
        b.Button(ID_QPAUSE, cx, r + 2 + btY, 96, L"Pause");
        b.Button(ID_QCANCEL, cx + 96 + 8, r + 2 + btY, 96, L"Cancel");
        setCol(margin + colW + colGap, colW);
        b.Label(lx, r, labelW, L"Waiting:", ID_QLIST);
        int qbW = b.ButtonW(L"Start all");
        for (const wchar_t* t : {L"Start", L"Remove"}) if (b.ButtonW(t) > qbW) qbW = b.ButtonW(t);
        const int lbW = cw - qbW - 8;
        b.Tip(b.Make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, cx, r,
                     lbW, listH, ID_QLIST, WS_EX_CLIENTEDGE),
              HelpText(ID_QLIST));
        const int bstep = (listH - 26) / 2;
        b.Button(ID_QSTART, cx + lbW + 8, r + 2, qbW, L"Start");
        b.Button(ID_QSTARTALL, cx + lbW + 8, r + 2 + bstep, qbW, L"Start all");
        b.Button(ID_QREMOVE, cx + lbW + 8, r + 2 + 2 * bstep, qbW, L"Remove");
        y += qH + gap;
    }
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
        if (he) SendMessageW(he, EM_GETRECT, 0, (LPARAM)&fr);
        if (create) g_helpPad = helpH > fr.bottom - fr.top ? helpH - (fr.bottom - fr.top) : 0;
        const int lines = (fr.bottom - fr.top) / lh;
        if (he && lines >= 1) {
            fr.bottom = fr.top + lines * lh;
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
    const int wOpen = b.ButtonW(L"Open config.ini"), wDef = b.ButtonW(L"Restore Defaults"), wOk = 96;
    if (create) g_contentEnd = g_made.size();   // g_made[0 .. g_contentEnd) = the content, the rest = the footer
    b.Button(ID_OPENINI, gx, by, wOpen, L"Open config.ini");
    b.Button(ID_DEFAULTS, gx + wOpen + 8, by, wDef, L"Restore Defaults");
    b.Button(ID_SAVE, gx + gw - 2 * wOk - 8, by, wOk, L"Save", true);
    b.Button(ID_CANCEL, gx + gw - wOk, by, wOk, L"Cancel");
    if (create) for (int id : {ID_OPENINI, ID_DEFAULTS, ID_SAVE, ID_CANCEL}) g_footer.push_back(GetDlgItem(dlg, id));
    HWND note = b.Make(L"STATIC", L"Changes apply to the next recording. Keyboard hotkeys and the indicator require a game restart.",
                       SS_LEFT | SS_NOPREFIX, gx, fy + 9, gw, 16, -1);
    keep(g_dim, note);
    keep(g_footer, note);
    // created last = lowest of the footer: it only paints the background behind the note and the buttons
    keep(g_footer, b.Make(L"STATIC", L"", SS_OWNERDRAW, 0, fy, clientW, g_footerH, kFooterBgId));
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

// The height the help edit needs for the longest help text (kOverview and every kHelp text) at the current (narrowest) width:
// DrawText DT_CALCRECT in the dialog font and the edit's own line count (its wrapping is the ground truth), the larger one.
// Sets g_helpMinH; run right after the Create pass (client kLayoutW wide).
void MeasureHelp() {
    HWND he = Ctl(ID_HELPTEXT);
    if (!he) return;
    Builder b{g_dlg};
    const int lh = b.LineH();
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
    g_helpMinH = maxLines * lh + g_helpPad;
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
        const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        const int view = ch - g_footerH;
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
            si.nPage = (UINT)ch;
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
        if (!(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;   // the dialog itself is not shown yet: not IsWindowVisible
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
        if (!_wcsicmp(cls, L"Static")) {
            if ((style & SS_TYPEMASK) == SS_OWNERDRAW || (style & SS_ELLIPSISMASK) || t.empty()) continue;
            ++checked;
            RECT need{0, 0, w, 0};
            DrawTextW(dc, t.c_str(), (int)t.size(), &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            if (need.bottom > h) report(c, t, need.bottom, h, "label (height: wraps to more lines than fit)");
        } else if (!_wcsicmp(cls, L"Button")) {
            if (t.empty()) continue;
            ++checked;
            const LONG type = style & BS_TYPEMASK;
            const int tw = textW(t);
            if (type == BS_GROUPBOX) { if (tw + 16 > w) report(c, t, tw + 16, w, "group box"); }
            else if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX) {
                const int need = tw + GetSystemMetrics(SM_CXMENUCHECK) + 4;
                if (need > w) report(c, t, need, w, "check box");
            } else if (tw + 12 > w) report(c, t, tw + 12, w, "button");
        } else if (!_wcsicmp(cls, L"ComboBox")) {
            RECT wr{};
            GetWindowRect(c, &wr);
            const int have = (wr.right - wr.left) - GetSystemMetrics(SM_CXVSCROLL) - 8;   // the closed combo: minus arrow and edges
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
        const int visible = (fr.bottom - fr.top) / Builder{g_dlg}.LineH();
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

void NotifyQueueChanged() {
    if (g_dlg) PostMessageW(g_dlg, WM_QUEUE_POKE, 0, 0);
}

void ShowDialog(const std::wstring& configPath, std::function<void(const std::vector<std::wstring>&)> onSaved) {
    if (g_dlg) {
        ShowWindow(g_dlg, SW_RESTORE);
        SetForegroundWindow(g_dlg);
        return;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // SHBrowseForFolder
    g_path = configPath;
    g_onSaved = onSaved;
    g_done = false;
    g_capturing = 0;

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -20;
        lf.lfWeight = FW_BOLD;
        g_logoFont = CreateFontIndirectW(&lf);
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"EyeCaptureSettings";
        RegisterClassW(&wc);
        registered = true;
    }
    g_scrollY = g_scrollMax = g_viewH = 0;
    g_made.clear();
    g_helpMinH = g_helpPad = g_naturalH = 0;
    RECT rc{0, 0, kLayoutW, 700};   // provisional: resized below to the height BuildControls computed
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_APPWINDOW);
    const int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
    g_dlg = CreateWindowExW(WS_EX_APPWINDOW, L"EyeCaptureSettings", kWindowTitle, style, sx < 0 ? 0 : sx, sy < 0 ? 0 : sy,
                            w, hgt, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_dlg) {
        // Create at the narrowest width, measure the help texts there, then one Move pass at the same provisional size: it
        // gives the natural content height with the measured Description panel (g_naturalH; independent of the window size).
        RECT pc{};
        GetClientRect(g_dlg, &pc);
        int natural = 0;
        BuildControls(g_dlg, Builder::Pass::Create, kLayoutW, pc.bottom - pc.top, 0, &natural);
        MeasureHelp();
        BuildControls(g_dlg, Builder::Pass::Move, kLayoutW, pc.bottom - pc.top, 0, &natural);
        g_naturalH = natural;
        RECT wa{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) wa = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        // the whole content without scrolling when the work area allows it; else as high as the work area (Relayout shows the
        // scroll bar; MinClientW() already leaves room for it)
        RECT fit{0, 0, MinClientW(), g_naturalH + g_footerH};
        AdjustWindowRectEx(&fit, style, FALSE, WS_EX_APPWINDOW);
        const int fw = fit.right - fit.left, maxH = wa.bottom - wa.top;
        const int fh = fit.bottom - fit.top < maxH ? fit.bottom - fit.top : maxH;
        const int fx = wa.left + (wa.right - wa.left - fw) / 2, fy = wa.top + (maxH - fh) / 2;
        SetWindowPos(g_dlg, nullptr, fx < wa.left ? wa.left : fx, fy < wa.top ? wa.top : fy, fw, fh, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();   // WM_SIZE did it already unless the size did not change
        SetTimer(g_dlg, kHoverTimer, 100, nullptr);
        SetTimer(g_dlg, kPulseTimer, 50, nullptr);
        SetTimer(g_dlg, kQueueTimer, 500, nullptr);
        LoadFromFile();
        UpdateQueueView();
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
            if (g_dlg && IsDialogMessageW(g_dlg, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (g_dlg) DestroyWindow(g_dlg);
    }
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_logoFont) { DeleteObject(g_logoFont); g_logoFont = nullptr; }
    g_dlg = nullptr;
    g_onSaved = nullptr;
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace settings
