// config -- config.ini parsing (key = value lines; '[sections]', ';' and '#' comments ignored).
//
// Lookup order: %APPDATA%\OpenXR-EyeCapture\config.ini if it exists, else config.ini next to the DLL.
// Unknown keys and bad values are logged and ignored (the default is kept).
#pragma once
#include <string>
#include "log.h"

struct Hotkey {
    unsigned vk = 0;          // virtual-key code of the main key
    bool ctrl = false, shift = false, alt = false;
    std::wstring text;        // normalised, e.g. "Ctrl+Shift+F10"
};

// "Ctrl+Shift+F10", "Alt+R", "Ctrl+Numpad5", "F9", "none" ... Modifiers must match exactly when pressed.
// "none" yields vk = 0 (hotkey disabled; the tray app still works).
bool ParseHotkey(const std::wstring& s, Hotkey& out);

enum class GammaMode { Auto, Srgb, Linear };

struct Config {
    Hotkey       hotkey;                 // default Ctrl+Shift+F10
    std::wstring outputDir;              // default <dll folder>\captures
    bool         hevc = true;            // codec = hevc | h264
    unsigned     bitrateMbps = 0;        // 0 = codec default (120 HEVC / 80 H.264)
    unsigned     fps = 90;               // fallback when the display refresh cannot be detected
    unsigned     maxFps = 0;             // 0 = record every frame the game submits; else drop frames above this rate
    unsigned     maxWidth = 0;           // 0 = native (no scaling); else scale the SBS frame down to this width
    bool         audioEnabled = true;
    std::wstring audioDevice;            // substring of the render endpoint name; empty = default render device
    GammaMode    gamma = GammaMode::Auto;
    bool         requireTray = true;     // require_tray: layer stays dormant unless EyeCaptureTray.exe is running
    bool         swapEyes = false;       // swap_eyes: put the right eye in the left half (for players that flip SBS)
    LogLevel     logLevel = LogLevel::Info;
    std::wstring loadedFrom;             // path of the ini that was read ("" = defaults only)
};

// dllDir: folder of eye_capture.dll (no trailing slash). Always fills `out` with defaults first.
void LoadConfig(const std::wstring& dllDir, Config& out);
// Path LoadConfig would read (may not exist): %APPDATA% copy if present, else <dllDir>\config.ini.
std::wstring ConfigPath(const std::wstring& dllDir);
