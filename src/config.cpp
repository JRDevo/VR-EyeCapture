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

struct KeyName { const wchar_t* name; unsigned vk; };
const KeyName kKeys[] = {
    {L"space", VK_SPACE}, {L"enter", VK_RETURN}, {L"tab", VK_TAB}, {L"backspace", VK_BACK},
    {L"insert", VK_INSERT}, {L"ins", VK_INSERT}, {L"delete", VK_DELETE}, {L"del", VK_DELETE},
    {L"home", VK_HOME}, {L"end", VK_END}, {L"pageup", VK_PRIOR}, {L"pgup", VK_PRIOR},
    {L"pagedown", VK_NEXT}, {L"pgdn", VK_NEXT}, {L"up", VK_UP}, {L"down", VK_DOWN}, {L"left", VK_LEFT},
    {L"right", VK_RIGHT}, {L"pause", VK_PAUSE}, {L"scrolllock", VK_SCROLL}, {L"printscreen", VK_SNAPSHOT},
    {L"numpadmultiply", VK_MULTIPLY}, {L"numpadadd", VK_ADD}, {L"numpadsubtract", VK_SUBTRACT},
    {L"numpaddivide", VK_DIVIDE}, {L"numpaddecimal", VK_DECIMAL},
};
} // namespace

bool ParseHotkey(const std::wstring& s, Hotkey& out) {
    Hotkey h;
    if (Lower(Trim(s)) == L"none") { h.text = L"none"; out = h; return true; }
    std::wstring rest = s;
    std::vector<std::wstring> parts;
    size_t pos;
    while ((pos = rest.find(L'+')) != std::wstring::npos) {
        parts.push_back(Trim(rest.substr(0, pos)));
        rest = rest.substr(pos + 1);
    }
    parts.push_back(Trim(rest));
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::wstring m = Lower(parts[i]);
        if (m == L"ctrl" || m == L"control") h.ctrl = true;
        else if (m == L"shift") h.shift = true;
        else if (m == L"alt") h.alt = true;
        else return false;
    }
    const std::wstring key = Lower(parts.back());
    unsigned vk = 0;
    if (key.size() == 1 && ((key[0] >= L'a' && key[0] <= L'z') || (key[0] >= L'0' && key[0] <= L'9'))) {
        vk = (unsigned)towupper(key[0]);
    } else if (key.size() >= 2 && key.size() <= 3 && key[0] == L'f') {
        unsigned n = 0;
        if (ParseUInt(key.substr(1), n) && n >= 1 && n <= 24) vk = VK_F1 + (n - 1);
    } else if (key.size() == 7 && key.compare(0, 6, L"numpad") == 0 && key[6] >= L'0' && key[6] <= L'9') {
        vk = VK_NUMPAD0 + (unsigned)(key[6] - L'0');
    } else {
        for (const auto& k : kKeys) if (key == k.name) { vk = k.vk; break; }
    }
    if (!vk) return false;
    h.vk = vk;
    if (h.ctrl) h.text += L"Ctrl+";
    if (h.shift) h.text += L"Shift+";
    if (h.alt) h.text += L"Alt+";
    h.text += parts.back();
    out = h;
    return true;
}

namespace {
enum class KeyResult { Ok, Bad, Unknown };
KeyResult ApplyKey(Config& c, const std::wstring& key, std::wstring val);
} // namespace

std::wstring ConfigPath(const std::wstring& dllDir) {
    wchar_t appdata[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring p = std::wstring(appdata) + L"\\OpenXR-EyeCapture\\config.ini";
        if (FileExists(p)) return p;
    }
    return dllDir + L"\\config.ini";
}

void LoadConfig(const std::wstring& dllDir, Config& c) {
    c = Config();
    ParseHotkey(L"Ctrl+Shift+F10", c.hotkey);
    c.outputDir = dllDir + L"\\captures";

    const std::wstring path = ConfigPath(dllDir);
    if (!FileExists(path)) {
        LOGW("config: no config.ini found (looked in %%APPDATA%%\\OpenXR-EyeCapture and %s); using defaults",
             elog::Narrow(dllDir).c_str());
        return;
    }
    c.loadedFrom = path;

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
        else if (r == KeyResult::Bad) LOGW("config line %d: bad value '%s' for '%s' (default kept)", lineNo, v8.c_str(), k8.c_str());
    }
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
        if (l == L"hevc" || l == L"h265" || l == L"h.265") c.hevc = true;
        else if (l == L"h264" || l == L"h.264" || l == L"avc") c.hevc = false;
        else ok = false;
    } else if (key == L"bitrate_mbps") {
        if (ParseUInt(val, u) && u <= 1000) c.bitrateMbps = u; else ok = false;
    } else if (key == L"fps") {
        if (ParseUInt(val, u) && u >= 10 && u <= 240) c.fps = u; else ok = false;
    } else if (key == L"max_fps") {
        if (ParseUInt(val, u) && u <= 240) c.maxFps = u; else ok = false;
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
    } else if (key == L"symmetric_fov") {
        if (ParseBool(val, b)) c.head.symmetricFov = b; else ok = false;
    } else if (key == L"log_level") {
        const std::wstring l = Lower(val);
        if (l == L"error") c.logLevel = LogLevel::Error;
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
