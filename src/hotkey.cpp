// hotkey -- see hotkey.h.
#include "hotkey.h"
#include <windows.h>
#include <cwctype>

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
bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}
// Decimal, no sign, at most 9 digits.
bool ParseDec(const std::wstring& v, unsigned& out) {
    if (v.empty() || v.size() > 9) return false;
    unsigned x = 0;
    for (wchar_t c : v) {
        if (c < L'0' || c > L'9') return false;
        x = x * 10 + (unsigned)(c - L'0');
    }
    out = x;
    return true;
}
bool ParseHex(const std::wstring& v, unsigned& out) {
    if (v.empty() || v.size() > 2) return false;
    unsigned x = 0;
    for (wchar_t c : v) {
        unsigned d;
        if (c >= L'0' && c <= L'9') d = (unsigned)(c - L'0');
        else if (c >= L'a' && c <= L'f') d = 10u + (unsigned)(c - L'a');
        else if (c >= L'A' && c <= L'F') d = 10u + (unsigned)(c - L'A');
        else return false;
        x = x * 16 + d;
    }
    out = x;
    return true;
}
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t pos = 0;
    for (;;) {
        const size_t k = s.find(sep, pos);
        parts.push_back(Trim(s.substr(pos, k == std::wstring::npos ? std::wstring::npos : k - pos)));
        if (k == std::wstring::npos) break;
        pos = k + 1;
    }
    return parts;
}

// ---- keyboard / mouse names (canonical spelling; existing names are unchanged from earlier versions)
struct KeyName { unsigned vk; const wchar_t* name; };
const KeyName kKeyNames[] = {
    {VK_LBUTTON, L"MouseLeft"}, {VK_RBUTTON, L"MouseRight"}, {VK_CANCEL, L"Break"}, {VK_MBUTTON, L"MouseMiddle"},
    {VK_XBUTTON1, L"MouseX1"}, {VK_XBUTTON2, L"MouseX2"},
    {VK_BACK, L"Backspace"}, {VK_TAB, L"Tab"}, {VK_CLEAR, L"Clear"}, {VK_RETURN, L"Enter"},
    {VK_PAUSE, L"Pause"}, {VK_CAPITAL, L"CapsLock"}, {VK_SPACE, L"Space"},
    {VK_PRIOR, L"PageUp"}, {VK_NEXT, L"PageDown"}, {VK_END, L"End"}, {VK_HOME, L"Home"},
    {VK_LEFT, L"Left"}, {VK_UP, L"Up"}, {VK_RIGHT, L"Right"}, {VK_DOWN, L"Down"},
    {VK_SNAPSHOT, L"PrintScreen"}, {VK_INSERT, L"Insert"}, {VK_DELETE, L"Delete"}, {VK_HELP, L"Help"},
    {VK_APPS, L"Apps"}, {VK_SLEEP, L"Sleep"},
    {VK_MULTIPLY, L"NumpadMultiply"}, {VK_ADD, L"NumpadAdd"}, {VK_SUBTRACT, L"NumpadSubtract"},
    {VK_DECIMAL, L"NumpadDecimal"}, {VK_DIVIDE, L"NumpadDivide"},
    {VK_NUMLOCK, L"NumLock"}, {VK_SCROLL, L"ScrollLock"},
    {VK_BROWSER_BACK, L"BrowserBack"}, {VK_BROWSER_FORWARD, L"BrowserForward"}, {VK_BROWSER_REFRESH, L"BrowserRefresh"},
    {VK_BROWSER_STOP, L"BrowserStop"}, {VK_BROWSER_SEARCH, L"BrowserSearch"}, {VK_BROWSER_FAVORITES, L"BrowserFavorites"},
    {VK_BROWSER_HOME, L"BrowserHome"},
    {VK_VOLUME_MUTE, L"VolumeMute"}, {VK_VOLUME_DOWN, L"VolumeDown"}, {VK_VOLUME_UP, L"VolumeUp"},
    {VK_MEDIA_NEXT_TRACK, L"MediaNext"}, {VK_MEDIA_PREV_TRACK, L"MediaPrev"}, {VK_MEDIA_STOP, L"MediaStop"},
    {VK_MEDIA_PLAY_PAUSE, L"MediaPlayPause"},
    {VK_LAUNCH_MAIL, L"LaunchMail"}, {VK_LAUNCH_MEDIA_SELECT, L"LaunchMediaSelect"}, {VK_LAUNCH_APP1, L"LaunchApp1"},
    {VK_LAUNCH_APP2, L"LaunchApp2"},
    {VK_OEM_1, L"Semicolon"}, {VK_OEM_PLUS, L"Equals"}, {VK_OEM_COMMA, L"Comma"}, {VK_OEM_MINUS, L"Minus"},
    {VK_OEM_PERIOD, L"Period"}, {VK_OEM_2, L"Slash"}, {VK_OEM_3, L"Backquote"}, {VK_OEM_4, L"LeftBracket"},
    {VK_OEM_5, L"Backslash"}, {VK_OEM_6, L"RightBracket"}, {VK_OEM_7, L"Quote"}, {VK_OEM_102, L"OEM102"},
};
// accepted when parsing, never produced
const KeyName kKeyAliases[] = {
    {VK_INSERT, L"ins"}, {VK_DELETE, L"del"}, {VK_PRIOR, L"pgup"}, {VK_NEXT, L"pgdn"},
};

// Keys the config cannot hold as the main key: Escape (cancels "Press a key"), the modifiers themselves, Windows keys.
bool Unnameable(unsigned vk) {
    switch (vk) {
    case 0: case VK_ESCAPE: case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        return true;
    default:
        return vk > 0xFF;
    }
}

std::wstring KeyName(unsigned vk) {
    if (Unnameable(vk)) return std::wstring();
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Numpad" + std::to_wstring(vk - VK_NUMPAD0);
    for (const auto& k : kKeyNames) if (k.vk == vk) return k.name;
    wchar_t b[16];
    swprintf_s(b, L"Key0x%02X", vk);
    return b;
}

unsigned KeyFromName(const std::wstring& lowerName) {
    const std::wstring& key = lowerName;
    if (key.empty()) return 0;
    if (key.size() == 1 && ((key[0] >= L'a' && key[0] <= L'z') || (key[0] >= L'0' && key[0] <= L'9'))) return (unsigned)towupper(key[0]);
    unsigned n = 0;
    if (key.size() >= 2 && key.size() <= 3 && key[0] == L'f' && ParseDec(key.substr(1), n) && n >= 1 && n <= 24) return VK_F1 + (n - 1);
    if (key.size() == 7 && StartsWith(key, L"numpad") && key[6] >= L'0' && key[6] <= L'9') return VK_NUMPAD0 + (unsigned)(key[6] - L'0');
    if (StartsWith(key, L"key0x") && ParseHex(key.substr(5), n)) return Unnameable(n) ? 0 : n;
    for (const auto& k : kKeyNames) if (key == Lower(k.name)) return k.vk;
    for (const auto& k : kKeyAliases) if (key == k.name) return k.vk;
    return 0;
}

// ---- XInput pad buttons, in canonical order
struct PadName { unsigned bit; const wchar_t* name; };
const PadName kPadNames[] = {
    {kPadA, L"A"}, {kPadB, L"B"}, {kPadX, L"X"}, {kPadY, L"Y"}, {kPadLB, L"LB"}, {kPadRB, L"RB"}, {kPadLT, L"LT"},
    {kPadRT, L"RT"}, {kPadLS, L"LS"}, {kPadRS, L"RS"}, {kPadBack, L"Back"}, {kPadStart, L"Start"}, {kPadGuide, L"Guide"},
    {kPadDUp, L"DUp"}, {kPadDDown, L"DDown"}, {kPadDLeft, L"DLeft"}, {kPadDRight, L"DRight"},
};
const wchar_t* const kPovNames[4] = {L"POVUp", L"POVDown", L"POVLeft", L"POVRight"};   // bit order kPovUp..kPovRight

bool ParseKeyBinding(const std::wstring& s, HotkeyBinding& b) {
    const std::vector<std::wstring> parts = Split(s, L'+');
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::wstring m = Lower(parts[i]);
        if (m == L"ctrl" || m == L"control") b.ctrl = true;
        else if (m == L"shift") b.shift = true;
        else if (m == L"alt") b.alt = true;
        else return false;
    }
    b.vk = KeyFromName(Lower(parts.back()));
    if (!b.vk) return false;
    b.kind = HotkeyBinding::Kind::Key;
    return KeyBindingText(b.vk, b.ctrl, b.shift, b.alt, b.text);
}

bool ParsePadBinding(const std::wstring& s, HotkeyBinding& b) {
    const size_t colon = s.find(L':');
    if (colon == std::wstring::npos) return false;
    const std::wstring prefix = Lower(Trim(s.substr(0, colon)));
    if (prefix == L"pad") b.padIndex = 0;
    else if (prefix.size() == 4 && StartsWith(prefix, L"pad") && prefix[3] >= L'1' && prefix[3] <= L'4') b.padIndex = (unsigned)(prefix[3] - L'0');
    else return false;
    for (const std::wstring& p : Split(s.substr(colon + 1), L'+')) {
        const std::wstring l = Lower(p);
        unsigned bit = 0;
        for (const auto& n : kPadNames) if (l == Lower(n.name)) bit = n.bit;
        if (!bit) return false;
        b.padButtons |= bit;
    }
    b.kind = HotkeyBinding::Kind::Pad;
    return PadBindingText(b.padIndex, b.padButtons, b.text);
}

bool ParseJoyBinding(const std::wstring& s, HotkeyBinding& b) {
    // Joy:<device>:<inputs>  (the device never contains ':')
    const size_t c1 = s.find(L':');
    const size_t c2 = c1 == std::wstring::npos ? std::wstring::npos : s.find(L':', c1 + 1);
    if (c2 == std::wstring::npos || s.find(L':', c2 + 1) != std::wstring::npos) return false;
    const std::wstring dev = SanitizeDeviceName(s.substr(c1 + 1, c2 - c1 - 1));
    if (dev.empty()) return false;
    b.joyDevice = dev;
    for (const std::wstring& p : Split(s.substr(c2 + 1), L'+')) {
        const std::wstring l = Lower(p);
        unsigned n = 0;
        if (StartsWith(l, L"button") && ParseDec(l.substr(6), n) && n >= 1 && n <= 128) {
            b.joyButtons[(n - 1) / 32] |= 1u << ((n - 1) % 32);
            continue;
        }
        bool pov = false;
        for (unsigned i = 0; i < 4; ++i)
            if (l == Lower(kPovNames[i])) { b.joyPov |= 1u << i; pov = true; }
        if (!pov) return false;
    }
    b.kind = HotkeyBinding::Kind::Joy;
    return JoyBindingText(b.joyDevice, b.joyButtons, b.joyPov, b.text);
}

bool ParseBinding(const std::wstring& s, HotkeyBinding& b) {
    const std::wstring l = Lower(s);
    if (StartsWith(l, L"joy:")) return ParseJoyBinding(s, b);
    if (StartsWith(l, L"pad") && l.find(L':') != std::wstring::npos) return ParsePadBinding(s, b);
    return ParseKeyBinding(s, b);
}
} // namespace

bool KeyBindingText(unsigned vk, bool ctrl, bool shift, bool alt, std::wstring& out) {
    const std::wstring key = KeyName(vk);
    if (key.empty()) return false;
    out.clear();
    if (ctrl) out += L"Ctrl+";
    if (shift) out += L"Shift+";
    if (alt) out += L"Alt+";
    out += key;
    return true;
}

bool PadBindingText(unsigned padIndex, unsigned padButtons, std::wstring& out) {
    if (padIndex > 4) return false;
    std::wstring t = padIndex ? L"Pad" + std::to_wstring(padIndex) + L":" : std::wstring(L"Pad:");
    bool any = false;
    for (const auto& n : kPadNames) {
        if (!(padButtons & n.bit)) continue;
        if (any) t += L"+";
        t += n.name;
        any = true;
    }
    if (!any) return false;
    out = t;
    return true;
}

bool JoyBindingText(const std::wstring& device, const unsigned buttons[4], unsigned pov, std::wstring& out) {
    const std::wstring dev = SanitizeDeviceName(device);
    if (dev.empty()) return false;
    std::wstring t = L"Joy:" + dev + L":";
    bool any = false;
    for (unsigned n = 0; n < 128; ++n) {
        if (!(buttons[n / 32] & (1u << (n % 32)))) continue;
        if (any) t += L"+";
        t += L"Button" + std::to_wstring(n + 1);
        any = true;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!(pov & (1u << i))) continue;
        if (any) t += L"+";
        t += kPovNames[i];
        any = true;
    }
    if (!any) return false;
    out = t;
    return true;
}

bool BindingText(const HotkeyBinding& b, std::wstring& out) {
    switch (b.kind) {
    case HotkeyBinding::Kind::Pad: return PadBindingText(b.padIndex, b.padButtons, out);
    case HotkeyBinding::Kind::Joy: return JoyBindingText(b.joyDevice, b.joyButtons, b.joyPov, out);
    default: return KeyBindingText(b.vk, b.ctrl, b.shift, b.alt, out);
    }
}

std::wstring SanitizeDeviceName(const std::wstring& productName) {
    std::wstring r;
    bool space = false;
    for (wchar_t c : productName) {
        if (c == L',' || c == L':' || c == L'+' || c == L';' || c == L'#' || iswspace(c) || c < 0x20) { space = true; continue; }
        if (space && !r.empty()) r += L' ';
        space = false;
        r += c;
    }
    return r;
}

bool JoyDeviceMatches(const std::wstring& bindingDevice, const std::wstring& sanitizedName) {
    if (bindingDevice == L"*") return true;
    if (bindingDevice.empty()) return false;
    return Lower(sanitizedName).find(Lower(bindingDevice)) != std::wstring::npos;
}

bool ParseHotkey(const std::wstring& s, Hotkey& out) {
    Hotkey h;
    const std::wstring all = Trim(s);
    if (Lower(all) == L"none") { h.text = L"none"; out = h; return true; }
    for (const std::wstring& item : Split(all, L',')) {
        if (item.empty()) return false;
        HotkeyBinding b;
        if (!ParseBinding(item, b)) return false;
        bool dup = false;
        for (const auto& e : h.bindings) if (e.text == b.text) dup = true;
        if (dup) continue;
        if (!h.text.empty()) h.text += L", ";
        h.text += b.text;
        if (b.kind == HotkeyBinding::Kind::Key && !h.vk) { h.vk = b.vk; h.ctrl = b.ctrl; h.shift = b.shift; h.alt = b.alt; }
        h.bindings.push_back(b);
    }
    if (h.bindings.empty()) return false;
    out = h;
    return true;
}
