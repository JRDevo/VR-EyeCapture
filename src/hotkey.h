// hotkey -- the `hotkey = ...` value: a comma-separated list of bindings, ANY of which toggles recording.
//
//   Key  (keyboard + mouse buttons; polled by the LAYER with GetAsyncKeyState, read when the game starts):
//        "Ctrl+Shift+F10", "ScrollLock", "MouseX1", "MediaPlayPause", "Key0xE5" (generic name of an unnamed key).
//        Modifiers Ctrl / Shift / Alt must match exactly.
//   Pad  (XInput / Xbox controllers; polled by the TRAY, applies at once):
//        "Pad:A", "Pad:Back+Start" (all held = chord), "Pad2:LB" (pad 1..4; "Pad:" = any pad).
//   Joy  (DirectInput game controllers: wheels, pedals, shifters, button boxes, HOTAS; polled by the TRAY):
//        "Joy:*:Button12", "Joy:T300RS:Button5+Button6", "Joy:<device>:POVUp" (first POV hat, Up / Down / Left / Right).
//        <device> = "*" (any) or a case-insensitive substring of the sanitised product name (SanitizeDeviceName).
//   "none" = no bindings (the tray menu still works).
// Names never contain '+', ',', ':' or spaces (device names never contain '+', ',' or ':'). Parsing is
// case-insensitive; every text produced here is the canonical spelling and parses back to the same binding.
#pragma once
#include <string>
#include <vector>

struct HotkeyBinding {
    enum class Kind { Key, Pad, Joy } kind = Kind::Key;
    // Key
    unsigned vk = 0;
    bool ctrl = false, shift = false, alt = false;
    // Pad: padIndex 0 = any pad, 1..4 = that XInput slot; padButtons = kPad* bits (XInput wButtons + LT / RT)
    unsigned padIndex = 0;
    unsigned padButtons = 0;
    // Joy: joyDevice "*" = any device; joyButtons = 128-bit mask (bit n = DirectInput button n+1); joyPov = kPov* bits
    std::wstring joyDevice;
    unsigned joyButtons[4] = {0, 0, 0, 0};
    unsigned joyPov = 0;
    std::wstring text;   // canonical text of this one binding
};

struct Hotkey {
    // The FIRST Key binding (vk = 0 when the list has none): kept for code that only knows one keyboard hotkey.
    unsigned vk = 0;          // virtual-key code of the main key
    bool ctrl = false, shift = false, alt = false;
    std::wstring text;        // the whole normalised list joined with ", " ("none" when empty), e.g. "ScrollLock, Pad:A"
    std::vector<HotkeyBinding> bindings;
};

// Pad button bits: the XInput wButtons values, plus LT / RT (trigger > 128) and Guide (XInputGetStateEx).
enum : unsigned {
    kPadDUp = 0x0001, kPadDDown = 0x0002, kPadDLeft = 0x0004, kPadDRight = 0x0008,
    kPadStart = 0x0010, kPadBack = 0x0020, kPadLS = 0x0040, kPadRS = 0x0080,
    kPadLB = 0x0100, kPadRB = 0x0200, kPadGuide = 0x0400,
    kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000,
    kPadLT = 0x10000, kPadRT = 0x20000,
};
// POV hat direction bits (Joy bindings).
enum : unsigned { kPovUp = 1, kPovDown = 2, kPovLeft = 4, kPovRight = 8 };

// "Ctrl+Shift+F10", "F9", "MouseX1", "Pad:A", "Joy:*:Button3", "ScrollLock, Pad:Back+Start", "none" ...
// False (out untouched) when ANY binding of the list is bad.
bool ParseHotkey(const std::wstring& s, Hotkey& out);

// Canonical texts. False when the input cannot be named (Escape, a bare modifier, vk 0, no buttons, ...).
bool KeyBindingText(unsigned vk, bool ctrl, bool shift, bool alt, std::wstring& out);
bool PadBindingText(unsigned padIndex, unsigned padButtons, std::wstring& out);
bool JoyBindingText(const std::wstring& device, const unsigned buttons[4], unsigned pov, std::wstring& out);
bool BindingText(const HotkeyBinding& b, std::wstring& out);

// DirectInput product name -> the form used in Joy bindings: ',' ':' '+' (and ';' '#', which config.ini could read as
// a comment) become spaces, runs of white space collapse to one space, trimmed.
std::wstring SanitizeDeviceName(const std::wstring& productName);
// Does a Joy binding's device ("*" or a substring) match this (already sanitised) device name? Case-insensitive.
bool JoyDeviceMatches(const std::wstring& bindingDevice, const std::wstring& sanitizedName);
