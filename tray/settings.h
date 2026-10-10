// settings -- the tray's "Settings..." window and the config.ini writer behind it (Win32 controls, no frameworks).
//
// The window edits the config file the tray / layer really use (ConfigPath(exe folder)): it rewrites only the
// `key = value` lines whose value changed, keeps comments, blank lines, section headers, unknown lines, the file's
// line endings and BOM, and appends a missing key at the end of its section. A candidate file is validated with the
// real parser (ParseConfigFile) before anything is written, so the rules live in one place (src/config.cpp).
// The layer re-reads the file at the start of every recording, so a saved change applies to the next recording.
#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "config.h"

namespace settings {

// key -> value text as it goes into the file ("1"/"0" for booleans, canonical enum names, plain numbers).
using KeyValues = std::vector<std::pair<std::wstring, std::wstring>>;

struct SaveResult {
    std::vector<std::wstring> problems;   // validation messages; non-empty = nothing was written
    std::vector<std::wstring> changed;    // "bitrate_mbps 0->20", "hotkey (added) Ctrl+Alt+R", ...
};

// The keys the window edits, taken from a Config (an empty hotkey means the default Ctrl+Shift+F10).
KeyValues FromConfig(const Config& c);
// Rewrites `path` in place (creates it if missing). Returns false when validation fails or the file cannot be written.
bool SaveKeyValuesToFile(const std::wstring& path, const KeyValues& kv, SaveResult& r);
bool SaveSettingsToFile(const std::wstring& path, const Config& c, SaveResult& r);

// Virtual key + modifiers -> the text ParseHotkey accepts ("Ctrl+Shift+F10"). False for keys it cannot name.
bool HotkeyText(unsigned vk, bool ctrl, bool shift, bool alt, std::wstring& out);

// Error reporting goes through one function: MessageBox by default; tests redirect it (tray: EYECAP_TRAY_TEST_SHM).
using Reporter = void (*)(const std::wstring& title, const std::wstring& text);
void SetReporter(Reporter r);

// Modal-style settings window (nested message loop; the tray keeps running). `onSaved` gets the changed-key list.
// Does nothing but focus the window if it is already open.
void ShowDialog(const std::wstring& configPath, std::function<void(const std::vector<std::wstring>& changed)> onSaved);
HWND DialogWindow();   // the open settings window, or nullptr

// Control IDs, so tests can drive the window through SendMessage.
enum : int {
    ID_HOTKEY = 2001, ID_HOTKEY_PICK, ID_CODEC, ID_BITRATE, ID_MAXFPS, ID_OUTDIR, ID_BROWSE, ID_AUDIO, ID_AUDIODEV,
    ID_PROJECTION, ID_FILL, ID_FILLFADE, ID_SYMFOV, ID_SWAP, ID_ROLL, ID_SMOOTH, ID_SMOOTHVAL, ID_FOVSCALE,
    ID_INDICATOR, ID_INDSIZE, ID_INDOPACITY, ID_POSTMKV, ID_KEEPMP4, ID_POSTFLAT, ID_REQTRAY, ID_LOGLEVEL,
    ID_SAVE, ID_CANCEL, ID_OPENINI, ID_DEFAULTS,
    ID_HELPTEXT,  // the read-only "What does this do?" panel (appended: existing ids do not move)
    ID_INDX, ID_INDY, ID_INDZ,  // indicator position (appended after ID_HELPTEXT for the same reason)
    ID_POSTYT8K,                // hidden check bound to post_yt8k (appended for the same reason)
    ID_YTMODE                   // "Make for YouTube" drop-down: drives the hidden ID_POSTMKV / ID_POSTYT8K checks
};
extern const wchar_t* const kWindowTitle;

} // namespace settings
