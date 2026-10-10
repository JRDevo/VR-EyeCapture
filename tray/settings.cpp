// settings -- see settings.h.
#include "settings.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "log.h"

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
};
const Field kFields[] = {
    {L"hotkey",            L"capture",         Kind::Edit,  ID_HOTKEY,     nullptr},
    {L"output_dir",        L"capture",         Kind::Edit,  ID_OUTDIR,     nullptr},
    {L"codec",             L"capture",         Kind::Combo, ID_CODEC,      L"hevc|h264", L"hevc (H.265)|h264"},
    {L"bitrate_mbps",      L"capture",         Kind::Edit,  ID_BITRATE,    nullptr},
    {L"max_fps",           L"capture",         Kind::Edit,  ID_MAXFPS,     nullptr},
    {L"require_tray",      L"capture",         Kind::Check, ID_REQTRAY,    nullptr},
    {L"swap_eyes",         L"capture",         Kind::Check, ID_SWAP,       nullptr},
    {L"symmetric_fov",     L"head_smoothing",  Kind::Check, ID_SYMFOV,     nullptr},
    {L"head_smoothing",    L"head_smoothing",  Kind::Track, ID_SMOOTH,     nullptr},
    {L"roll_lock",         L"head_smoothing",  Kind::Check, ID_ROLL,       nullptr},
    {L"output_fov_scale",  L"head_smoothing",  Kind::Edit,  ID_FOVSCALE,   nullptr},
    {L"projection",        L"dome",            Kind::Combo, ID_PROJECTION, L"flat|vr180|vr360", L"flat (side-by-side 3D)|vr180 (experimental)|vr360 (experimental)"},
    {L"fill",              L"dome",            Kind::Combo, ID_FILL,       L"black|last_seen", L"black (live view only)|last_seen (keep older view)"},
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
    {L"log_level",         L"debug",           Kind::Combo, ID_LOGLEVEL,   L"none|error|warn|info|debug", L"none (no logging)|error|warn|info|debug"},
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
    if (f->kind == Kind::Track || key == L"bitrate_mbps" || key == L"max_fps" || key == L"output_fov_scale" ||
        key == L"fill_fade_s" || key == L"indicator_x" || key == L"indicator_y" || key == L"indicator_z" ||
        key == L"indicator_size" || key == L"indicator_opacity") {
        wchar_t* end = nullptr;
        const double d = wcstod(v.c_str(), &end);
        if (!v.empty() && end && *end == L'\0') return FloatText(d);
        return Lower(v);
    }
    if (key == L"codec") {
        const std::wstring l = Lower(v);
        if (l == L"h265" || l == L"h.265") return L"hevc";
        if (l == L"h.264" || l == L"avc") return L"h264";
        return l;
    }
    if (key == L"log_level") {
        const std::wstring l = Lower(v);
        return l == L"warning" ? L"warn" : (l == L"off" ? L"none" : l);
    }
    if (key == L"projection" || key == L"fill") return Lower(v);
    if (key == L"hotkey") {
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
    kv.emplace_back(L"codec", c.hevc ? L"hevc" : L"h264");
    kv.emplace_back(L"bitrate_mbps", std::to_wstring(c.bitrateMbps));
    kv.emplace_back(L"max_fps", std::to_wstring(c.maxFps));
    kv.emplace_back(L"require_tray", B(c.requireTray));
    kv.emplace_back(L"swap_eyes", B(c.swapEyes));
    kv.emplace_back(L"symmetric_fov", B(c.head.symmetricFov));
    kv.emplace_back(L"head_smoothing", FloatText(c.head.smoothing));
    kv.emplace_back(L"roll_lock", B(c.head.rollLock));
    kv.emplace_back(L"output_fov_scale", FloatText(c.head.fovScale));
    kv.emplace_back(L"projection", FromUtf8(DomeProjectionName(c.dome.projection)));
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
                r.problems.push_back(L"hotkey: '" + v + L"' is not a valid hotkey (examples: Ctrl+Shift+F10, F9, Alt+R, Ctrl+Numpad5, none)");
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
    for (const auto& p : newProblems) {
        bool known = false;
        for (const auto& o : oldProblems) if (o == p) known = true;
        if (!known) r.problems.push_back(p);
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
    std::wstring key;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) key = std::wstring(1, (wchar_t)vk);
    else if (vk >= VK_F1 && vk <= VK_F24) key = L"F" + std::to_wstring(vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) key = L"Numpad" + std::to_wstring(vk - VK_NUMPAD0);
    else {
        static const struct { unsigned vk; const wchar_t* name; } kNames[] = {
            {VK_SPACE, L"Space"}, {VK_RETURN, L"Enter"}, {VK_TAB, L"Tab"}, {VK_BACK, L"Backspace"},
            {VK_INSERT, L"Insert"}, {VK_DELETE, L"Delete"}, {VK_HOME, L"Home"}, {VK_END, L"End"},
            {VK_PRIOR, L"PageUp"}, {VK_NEXT, L"PageDown"}, {VK_UP, L"Up"}, {VK_DOWN, L"Down"}, {VK_LEFT, L"Left"},
            {VK_RIGHT, L"Right"}, {VK_PAUSE, L"Pause"}, {VK_SCROLL, L"ScrollLock"}, {VK_SNAPSHOT, L"PrintScreen"},
            {VK_MULTIPLY, L"NumpadMultiply"}, {VK_ADD, L"NumpadAdd"}, {VK_SUBTRACT, L"NumpadSubtract"},
            {VK_DIVIDE, L"NumpadDivide"}, {VK_DECIMAL, L"NumpadDecimal"},
        };
        for (const auto& k : kNames) if (k.vk == vk) key = k.name;
    }
    if (key.empty()) return false;
    out.clear();
    if (ctrl) out += L"Ctrl+";
    if (shift) out += L"Shift+";
    if (alt) out += L"Alt+";
    out += key;
    return true;
}

// ================================================================================================ the window
namespace {

HWND                g_dlg = nullptr;
HWND                g_tip = nullptr;            // the tooltip window (owned by the dialog)
HFONT               g_font = nullptr;
HWND                g_hover = nullptr;          // the control the help panel last followed (mouse or keyboard focus)
const wchar_t*      g_shown = nullptr;          // the text now in the help panel (pointer compare: no flicker)
std::wstring        g_path;
bool                g_done = false;
bool                g_capturing = false;
int                 g_smoothPos = -1;       // trackbar position / text as loaded: an untouched slider keeps the exact file value
std::wstring        g_smoothText;
std::function<void(const std::vector<std::wstring>&)> g_onSaved;


// ---- help texts: hover tooltip + the "What does this do?" panel. Plain words, short sentences.
const wchar_t* const kOverview =
    L"Point at a setting to read what it does.\r\n"
    L"How it works: eye_capture.dll is an OpenXR layer. While this tray app runs and you press the hotkey, the layer copies the "
    L"two eye images the game sends to the headset, encodes them on the GPU and writes an MP4 (and then an MKV for YouTube). "
    L"Nothing is injected into the game. Saved changes apply to the next recording.";

struct Help { int id; const wchar_t* text; };
const Help kHelp[] = {
    {ID_HOTKEY, L"The key that starts and stops a recording while the game runs. Press it once to start, press it again to stop. "
                L"Pick a key the game does not use (ScrollLock works well). Read when the game starts: restart the game after you change it."},
    {ID_HOTKEY_PICK, L"Click this, then press the key or key combination you want. Escape cancels."},
    {ID_CODEC, L"Video compression. hevc (H.265) gives the best picture per megabyte and is what YouTube wants for big 3D video. "
               L"h264 is older and plays in more programs, but files get bigger or softer."},
    {ID_BITRATE, L"Megabits per second of video. Higher = sharper picture and bigger files. 0 = the recorder's default: 120 for hevc, 80 for h264."},
    {ID_MAXFPS, L"Caps the frames per second in the recording. 0 = record every frame the headset shows (72 or 90). "
                L"60 or 30 makes smaller files and less GPU work. YouTube plays 60."},
    {ID_OUTDIR, L"Where recordings are saved. Empty = the 'captures' folder next to eye_capture.dll."},
    {ID_BROWSE, L"Pick the output folder."},
    {ID_AUDIO, L"Records what you hear (the Windows playback device) into the video."},
    {ID_AUDIODEV, L"Part of the playback device name to record, for example 'Virtual Desktop Audio'. Empty = the Windows default playback device."},
    {ID_PROJECTION, L"flat = normal side-by-side 3D video. The viewer sees exactly what you saw, in 3D. Recommended; plays everywhere.\r\n"
                    L"vr180 / vr360 are EXPERIMENTAL. The video becomes a dome you can look around in with a VR player. "
                    L"But the game only draws what your headset looks at (about 100 degrees). The dome shows live video there, "
                    L"older video where you looked before, and black where you never looked. It is not a full live 360 world. "
                    L"Needs a VR180 / VR360 player (YouTube VR, DeoVR, Virtual Desktop Videos). "
                    L"Symmetric field of view and head smoothing are not used in dome mode."},
    {ID_FILL, L"Dome mode (vr180 / vr360) only. last_seen = what you looked at earlier stays on the dome until it fades. "
              L"black = only the live view; everything else is black."},
    {ID_FILLFADE, L"Dome mode only. How many seconds an older area stays before it fades to black (the fade itself takes 2 seconds). 0 = never fade."},
    {ID_SYMFOV, L"Flat mode only. The headset draws each eye with a lopsided field of view (wider on the outside). "
                L"This re-projects both eyes to one symmetric view, so a 3D player can fuse them without eye strain. "
                L"Costs one GPU pass per frame. Turn it off only to record the raw eye images."},
    {ID_SWAP, L"Puts the right eye on the left half and the left eye on the right half. Use it if the 3D looks inside-out."},
    {ID_ROLL, L"Flat mode, with head smoothing above 0. Keeps the horizon level in the video when you tilt your head."},
    {ID_SMOOTH, L"Flat mode only. 0 = off: the video follows your head exactly. Higher = small head shakes are removed; "
                L"each frame is re-projected against a smoothed head direction. Big turns still follow your head. "
                L"Costs one GPU pass per frame. Black edges are prevented automatically (edge_guard in config.ini, on by default)."},
    {ID_FOVSCALE, L"Flat mode, with head smoothing on. The video shows this fraction of the game's field of view (0.5 .. 1). "
                  L"The trimmed border is the room the smoothing correction needs at the edges. 1 = no room, "
                  L"0.9 = about 3 degrees on every side (default). Smaller = more room, less view."},
    {ID_INDICATOR, L"Shows a small red dot in the headset while recording, so you know it is on. It is not in the video. "
                   L"Direct3D 11 games only (ETS2 / ATS). Read when the game starts."},
    {ID_INDX, L"Where the dot sits, left/right of straight ahead, in metres. 0 = centre, + = right, - = left. Default 0.22."},
    {ID_INDY, L"Where the dot sits, up/down, in metres. 0 = eye level, + = up, - = down. Default -0.16 (a little below centre)."},
    {ID_INDZ, L"How far in front of you the dot is, in metres. Always negative (-0.8 = 80 cm ahead). Further away = the same size looks smaller. Default -0.8."},
    {ID_INDSIZE, L"Edge length of the dot in metres at its distance Z. At -0.8 m: 0.02 = about 1.4 degrees, 0.005 = a small dot, "
                 L"0.002 = a few pixels. Below about 0.001 it is smaller than one headset pixel and disappears. Smallest allowed 0.0005."},
    {ID_INDOPACITY, L"How solid the red dot is. 1 = fully visible, 0.3 = faint."},
    {ID_POSTMKV, L"After a recording the tray copies the video into an .mkv with the 3D tags YouTube reads "
                 L"(the MP4 tags are ignored by YouTube). No re-encoding; takes seconds. Upload the .mkv."},
    {ID_KEEPMP4, L"Keep the original .mp4 next to the .mkv. Off = the .mp4 is deleted once the .mkv is made."},
    {ID_POSTFLAT, L"Also makes a normal 2D video from the left eye (_FLAT.mp4) for viewers without 3D. "
                  L"This re-encodes with ffmpeg and takes a while."},
    {ID_REQTRAY, L"The layer stays asleep unless this tray app runs. Keep it on, so a game never records by surprise. Read when the game starts."},
    {ID_LOGLEVEL, L"How much logs\\eye_capture.log and logs\\tray.log write. none = nothing at all. info is normal. debug is for bug reports."},
    {ID_OPENINI, L"Opens config.ini in a text editor. The file has every setting, with comments."},
    {ID_DEFAULTS, L"Puts every field back to the built-in default. Nothing is saved until you click Save."},
    {ID_SAVE, L"Writes the changed values into config.ini. The next recording uses them."},
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
            size_t pos = 0;
            while (pos <= opts.size()) {
                size_t bar = opts.find(L'|', pos);
                if (bar == std::wstring::npos) bar = opts.size();
                if (opts.substr(pos, bar - pos) == cur) sel = idx;
                ++idx;
                pos = bar + 1;
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
}

KeyValues CollectControls() {
    KeyValues kv;
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
    if (!problems.empty()) {
        std::wstring m = L"These lines of the config file have a bad value. The window shows the default for them, and Save replaces them:\n\n";
        for (const auto& p : problems) m += p + L"\n";
        Report(L"Settings", m);
    }
}

void ArmCapture(bool on) {
    g_capturing = on;
    SetWindowTextW(Ctl(ID_HOTKEY_PICK), on ? L"Press the key... (Esc = cancel)" : L"Press a key...");
}

void DoSave() {
    SaveResult r;
    if (!SaveKeyValuesToFile(g_path, CollectControls(), r)) {
        std::wstring m = L"Nothing was saved:\n\n";
        for (const auto& p : r.problems) m += p + L"\n";
        Report(L"Settings -- not saved", m);
        return;
    }
    if (g_onSaved) g_onSaved(r.changed);
    DestroyWindow(g_dlg);
}

void DoBrowse() {
    wchar_t disp[MAX_PATH];
    BROWSEINFOW bi{};
    bi.hwndOwner = g_dlg;
    bi.pszDisplayName = disp;
    bi.lpszTitle = L"Folder for the recordings";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t p[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, p)) SetWindowTextW(Ctl(ID_OUTDIR), p);
        CoTaskMemFree(pidl);
    }
}

void CommandHandler(int id, int code) {
    // keyboard focus (tab / click) shows the help of that control; buttons only send BN_SETFOCUS with BS_NOTIFY
    if (code == EN_SETFOCUS || code == CBN_SETFOCUS || code == BN_SETFOCUS) {
        if (id != ID_HELPTEXT) { ShowHelp(HelpText(id)); g_hover = Ctl(id); }
        return;
    }
    if (id == ID_PROJECTION && code == CBN_SELCHANGE) { ShowHelp(HelpText(ID_PROJECTION)); return; }
    if ((id == ID_SAVE || id == ID_CANCEL || id == ID_OPENINI || id == ID_DEFAULTS || id == ID_BROWSE) && code != BN_CLICKED) return;
    switch (id) {
    case ID_SAVE: DoSave(); break;
    case ID_CANCEL: DestroyWindow(g_dlg); break;
    case ID_OPENINI: ShellExecuteW(nullptr, L"open", g_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_DEFAULTS: {
        Config d;
        ParseHotkey(L"Ctrl+Shift+F10", d.hotkey);
        SetControls(FromConfig(d));
        break;
    }
    case ID_BROWSE: DoBrowse(); break;
    case ID_HOTKEY_PICK:
        if (code == BN_CLICKED) { ArmCapture(!g_capturing); if (g_capturing) SetFocus(Ctl(ID_HOTKEY_PICK)); }
        break;
    default: break;
    }
}

// Called from the message loop for key messages while "Press a key" is armed. True = message consumed.
bool CaptureKey(const MSG& m) {
    if (!g_capturing || (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN)) return false;
    const unsigned vk = (unsigned)m.wParam;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
        vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU)
        return true;   // wait for the main key
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0,
               alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (vk == VK_ESCAPE && !ctrl && !shift && !alt) { ArmCapture(false); return true; }
    std::wstring t;
    if (!HotkeyText(vk, ctrl, shift, alt, t)) {
        MessageBeep(MB_ICONEXCLAMATION);
        return true;   // keep waiting for a key the config can name
    }
    SetWindowTextW(Ctl(ID_HOTKEY), t.c_str());
    ArmCapture(false);
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
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g_capturing) ArmCapture(false);
        return 0;
    case WM_SETCURSOR:
        // standard controls forward this to the parent: wp = the child under the mouse (or the dialog itself)
        Hover((HWND)wp);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_TIMER:
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
        if ((HWND)lp == Ctl(ID_HELPTEXT)) {   // the read-only help edit: same background as the dialog
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, kHoverTimer);
        g_done = true;
        g_dlg = nullptr;
        g_tip = nullptr;
        g_hover = nullptr;
        g_shown = nullptr;
        PostMessageW(nullptr, WM_NULL, 0, 0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

struct Builder {
    HWND parent;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int hgt, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hgt, parent, (HMENU)(INT_PTR)id,
                                 GetModuleHandleW(nullptr), nullptr);
        if (g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        return c;
    }
    // helpId != 0: hovering the label shows the help of that control (the label keeps the id in GWLP_USERDATA)
    void Label(int x, int y, int w, const wchar_t* t, int helpId = 0) {
        HWND c = Make(L"STATIC", t, SS_LEFT | (helpId ? SS_NOTIFY : 0), x, y + 3, w, 18, -1);
        if (helpId) {
            SetWindowLongPtrW(c, GWLP_USERDATA, helpId);
            AddTip(c, HelpText(helpId));
        }
    }
    void Edit(int id, int x, int y, int w) {
        AddTip(Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 22, id, WS_EX_CLIENTEDGE), HelpText(id));
    }
    void Check(int id, int x, int y, int w, const wchar_t* t) {
        AddTip(Make(L"BUTTON", t, WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY, x, y + 1, w, 20, id), HelpText(id));
    }
    void Button(int id, int x, int y, int w, const wchar_t* t, bool def = false) {
        AddTip(Make(L"BUTTON", t, WS_TABSTOP | BS_NOTIFY | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y, w, 24, id), HelpText(id));
    }
    void Group(int x, int y, int w, int hgt, const wchar_t* t) { Make(L"BUTTON", t, BS_GROUPBOX, x, y, w, hgt, -1); }
    // The items come from the Field (display labels when it has them): the values written to config.ini are mapped by INDEX.
    void Combo(int id, int x, int y, int w) {
        const Field* f = FindFieldById(id);
        HWND c = Make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 120, id);
        AddTip(c, HelpText(id));
        if (!f) return;
        SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)(w > 190 ? w : 190), 0);
        const std::wstring o = f->labels ? f->labels : f->options;
        size_t pos = 0;
        while (pos <= o.size()) {
            size_t bar = o.find(L'|', pos);
            if (bar == std::wstring::npos) bar = o.size();
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)o.substr(pos, bar - pos).c_str());
            pos = bar + 1;
        }
    }
};

// Creates all controls; returns the client height the window needs.
int BuildControls(HWND dlg) {
    Builder b{dlg};
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_tip) {
        SetWindowPos(g_tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 380);
        SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(30000, 0));
    }
    const int gx = 10, gw = 570, ix = 22, row = 28;
    int y = 8;
    b.Make(L"STATIC", (L"Editing: " + g_path).c_str(), SS_LEFT | SS_PATHELLIPSIS, gx, y, gw, 18, -1);
    y += 24;

    // ---- Recording
    b.Group(gx, y, gw, 22 + 4 * row + 6, L"Recording");
    int r = y + 20;
    b.Label(ix, r, 60, L"Hotkey", ID_HOTKEY);
    b.Edit(ID_HOTKEY, ix + 65, r, 190);
    b.Button(ID_HOTKEY_PICK, ix + 265, r - 1, 190, L"Press a key...");
    r += row;
    b.Label(ix, r, 50, L"Codec", ID_CODEC);
    b.Combo(ID_CODEC, ix + 55, r, 100);
    b.Label(ix + 165, r, 150, L"Bitrate Mb/s (0 = default)", ID_BITRATE);
    b.Edit(ID_BITRATE, ix + 318, r, 50);
    b.Label(ix + 385, r, 90, L"Max fps (0 = all)", ID_MAXFPS);
    b.Edit(ID_MAXFPS, ix + 480, r, 50);
    r += row;
    b.Label(ix, r, 85, L"Output folder", ID_OUTDIR);
    b.Edit(ID_OUTDIR, ix + 90, r, 350);
    b.Button(ID_BROWSE, ix + 450, r - 1, 90, L"Browse...");
    r += row;
    b.Check(ID_AUDIO, ix, r, 120, L"Record audio");
    b.Label(ix + 135, r, 110, L"Device name has", ID_AUDIODEV);
    b.Edit(ID_AUDIODEV, ix + 250, r, 280);
    y += 22 + 4 * row + 6 + 6;

    // ---- 3D
    b.Group(gx, y, gw, 22 + 4 * row + 6, L"3D");
    r = y + 20;
    b.Label(ix, r, 65, L"Projection", ID_PROJECTION);
    b.Combo(ID_PROJECTION, ix + 68, r, 135);
    b.Label(ix + 215, r, 25, L"Fill", ID_FILL);
    b.Combo(ID_FILL, ix + 242, r, 165);
    b.Label(ix + 420, r, 50, L"Fade (s)", ID_FILLFADE);
    b.Edit(ID_FILLFADE, ix + 472, r, 55);
    r += row;
    b.Check(ID_SYMFOV, ix, r, 170, L"Symmetric field of view");
    b.Check(ID_SWAP, ix + 190, r, 100, L"Swap eyes");
    b.Check(ID_ROLL, ix + 310, r, 120, L"Roll lock");
    r += row;
    b.Label(ix, r, 110, L"Head smoothing", ID_SMOOTH);
    HWND tb = b.Make(L"msctls_trackbar32", L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, ix + 115, r - 3, 300, 28, ID_SMOOTH);
    AddTip(tb, HelpText(ID_SMOOTH));
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(0, 20));
    SendMessageW(tb, TBM_SETTICFREQ, 2, 0);
    SendMessageW(tb, TBM_SETPAGESIZE, 0, 2);
    SendMessageW(tb, TBM_SETLINESIZE, 0, 1);
    b.Make(L"STATIC", L"0.00", SS_LEFT, ix + 425, r + 3, 50, 18, ID_SMOOTHVAL);
    r += row;
    b.Label(ix, r, 190, L"Output FOV scale (0.5 .. 1)", ID_FOVSCALE);
    b.Edit(ID_FOVSCALE, ix + 195, r, 55);
    y += 22 + 4 * row + 6 + 6;

    // ---- indicator
    b.Group(gx, y, gw, 22 + 2 * row + 6, L"In-headset indicator");
    r = y + 20;
    b.Check(ID_INDICATOR, ix, r, 150, L"Show the red dot");
    b.Label(ix + 190, r, 85, L"Size (metres)", ID_INDSIZE);
    b.Edit(ID_INDSIZE, ix + 280, r, 55);
    b.Label(ix + 360, r, 100, L"Opacity (0 .. 1)", ID_INDOPACITY);
    b.Edit(ID_INDOPACITY, ix + 465, r, 55);
    r += row;
    b.Label(ix, r, 95, L"Position X (m)", ID_INDX);
    b.Edit(ID_INDX, ix + 100, r, 55);
    b.Label(ix + 190, r, 35, L"Y (m)", ID_INDY);
    b.Edit(ID_INDY, ix + 230, r, 55);
    b.Label(ix + 360, r, 35, L"Z (m)", ID_INDZ);
    b.Edit(ID_INDZ, ix + 400, r, 55);
    y += 22 + 2 * row + 6 + 6;

    // ---- after recording
    b.Group(gx, y, gw, 22 + row + 6, L"After recording");
    r = y + 20;
    b.Check(ID_POSTMKV, ix, r, 170, L"Make a .mkv (YouTube 3D)");
    b.Check(ID_KEEPMP4, ix + 190, r, 150, L"Keep the .mp4");
    b.Check(ID_POSTFLAT, ix + 350, r, 190, L"Also make a flat 2D copy");
    y += 22 + row + 6 + 6;

    // ---- advanced
    b.Group(gx, y, gw, 22 + row + 6, L"Advanced");
    r = y + 20;
    b.Check(ID_REQTRAY, ix, r, 330, L"Only record while this tray app is running");
    b.Label(ix + 370, r, 65, L"Log level", ID_LOGLEVEL);
    b.Combo(ID_LOGLEVEL, ix + 440, r, 80);
    y += 22 + row + 6 + 6;

    // ---- description panel (follows the mouse / keyboard focus, see Hover / ShowHelp)
    const int helpH = 112;
    b.Group(gx, y, gw, 20 + helpH + 8, L"What does this do?");
    b.Make(L"EDIT", kOverview, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ix, y + 18, gw - 24, helpH, ID_HELPTEXT);
    g_shown = kOverview;
    y += 20 + helpH + 8 + 6;

    b.Make(L"STATIC", L"Saving applies to the NEXT recording. The hotkey, 'only record while the tray runs' and the indicator are read when the game starts: restart the game for those.",
           SS_LEFT, gx, y, gw, 32, -1);
    y += 38;
    b.Button(ID_OPENINI, gx, y, 110, L"Open config.ini");
    b.Button(ID_DEFAULTS, gx + 118, y, 120, L"Restore defaults");
    b.Button(ID_SAVE, gx + gw - 170, y, 80, L"Save", true);
    b.Button(ID_CANCEL, gx + gw - 82, y, 80, L"Cancel");
    return y + 24 + 10;
}

} // namespace

HWND DialogWindow() { return g_dlg; }

void ShowDialog(const std::wstring& configPath, std::function<void(const std::vector<std::wstring>&)> onSaved) {
    if (g_dlg) {
        ShowWindow(g_dlg, SW_RESTORE);
        SetForegroundWindow(g_dlg);
        return;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // SHBrowseForFolder
    g_path = configPath;
    g_onSaved = onSaved;
    g_done = false;
    g_capturing = false;

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) g_font = CreateFontIndirectW(&ncm.lfMessageFont);

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
    RECT rc{0, 0, 590, 700};   // provisional: resized below to the height BuildControls computed
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_APPWINDOW);
    const int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
    g_dlg = CreateWindowExW(WS_EX_APPWINDOW, L"EyeCaptureSettings", kWindowTitle, style, sx < 0 ? 0 : sx, sy < 0 ? 0 : sy,
                            w, hgt, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_dlg) {
        const int clientH = BuildControls(g_dlg);
        RECT fit{0, 0, 590, clientH};
        AdjustWindowRectEx(&fit, style, FALSE, WS_EX_APPWINDOW);
        const int fw = fit.right - fit.left, fh = fit.bottom - fit.top;
        const int fx = (GetSystemMetrics(SM_CXSCREEN) - fw) / 2, fy = (GetSystemMetrics(SM_CYSCREEN) - fh) / 2;
        SetWindowPos(g_dlg, nullptr, fx < 0 ? 0 : fx, fy < 0 ? 0 : fy, fw, fh, SWP_NOZORDER | SWP_NOACTIVATE);
        SetTimer(g_dlg, kHoverTimer, 100, nullptr);
        LoadFromFile();
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
    g_dlg = nullptr;
    g_onSaved = nullptr;
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace settings
