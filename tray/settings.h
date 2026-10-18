// settings -- the tray's main window (settings, conversion queue, export) and the config.ini writer behind it (Win32 controls, no frameworks).
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
#include "postprocess.h"

namespace settings {

// key -> value text as it goes into the file ("1"/"0" for booleans, canonical enum names, plain numbers).
using KeyValues = std::vector<std::pair<std::wstring, std::wstring>>;

struct SaveResult {
    std::vector<std::wstring> problems;   // validation messages; non-empty = nothing was written
    std::vector<std::wstring> changed;    // "bitrate_mbps 0->20", "hotkey (added) Ctrl+Alt+R", ...
};

// The keys the window edits, taken from a Config (an empty hotkey means the default, kDefaultHotkey = ScrollLock).
KeyValues FromConfig(const Config& c);
// Rewrites `path` in place (creates it if missing). Returns false when validation fails or the file cannot be written.
bool SaveKeyValuesToFile(const std::wstring& path, const KeyValues& kv, SaveResult& r);
bool SaveSettingsToFile(const std::wstring& path, const Config& c, SaveResult& r);

// Virtual key + modifiers -> the text ParseHotkey accepts ("Ctrl+Shift+F10", "MouseX1", "Key0xE5"). False only for Escape,
// the modifier / Windows keys themselves and vk 0. Same table as KeyBindingText (src/hotkey.h).
bool HotkeyText(unsigned vk, bool ctrl, bool shift, bool alt, std::wstring& out);

// Error reporting goes through one function: MessageBox by default; tests redirect it (tray: EYECAP_TRAY_TEST_SHM).
using Reporter = void (*)(const std::wstring& title, const std::wstring& text);
void SetReporter(Reporter r);

// Modal-style settings window (nested message loop; the tray keeps running). `onSaved` gets the changed-key list.
// Does nothing but focus the window if it is already open.
void ShowDialog(const std::wstring& configPath, std::function<void(const std::vector<std::wstring>& changed)> onSaved);
HWND DialogWindow();   // the open settings window, or nullptr

// The "Conversions" group (queue / progress view) and the "Convert a recording..." / "Add to queue..." buttons talk to the
// tray's conversion worker through these callbacks (the window runs on the tray's UI thread, so they are called on that
// thread). The tray sets them once before the first ShowDialog; an unset hook disables its control (settings_test links the
// window without a worker).
struct QueueHooks {
    std::function<pp::State()>                   state;           // pp::Processor::GetState
    std::function<std::vector<pp::QueuedJob>()>  queued;          // pp::Processor::Queued (jobs without a snapshot: fmt = config.ini's)
    // the multi-select MP4 Open dialog; each file queued with `fmt` (the values the window shows, saved or not):
    // start = forced ("Convert a recording..."), else held by the user ("Add to queue...")
    std::function<void(HWND owner, const pp::JobFormat& fmt, bool start)> pickFiles;
    std::function<void()>                        togglePause;     // pause / resume the running job
    std::function<void()>                        cancel;          // cancel the running job
    std::function<void(unsigned long long id)>   start;           // force a waiting job (or the running, held one)
    std::function<void()>                        startAll;        // force every waiting job (they run oldest first)
    std::function<void(unsigned long long id)>   remove;          // drop a waiting job
    std::function<unsigned()>                    gameFps;         // the running game's frame rate (status block), 0 = no game / unknown
};
void SetQueueHooks(const QueueHooks& hooks);
// The footer button "SteamVR games..." calls this (the tray sets its own "SteamVR games" opener, so there is one window in one process;
// it brings the window to the front when open). Unset (settings_test without a tray) = the button is greyed.
void SetOpenSteamVrHook(std::function<void()> hook);
// The Advanced group's "Tools:" row (these used to be tray menu items). Each unset hook greys its button.
struct ToolHooks {
    std::function<void()>               openLog;      // opens logs\eye_capture.log
    std::function<void(HWND, bool)>     tag3d;        // file dialog (owner), then tags the picked video: false = side by side, true = top / bottom
    std::function<int()>                regState;     // 0 = no button (registration not handled here), 1 = "Register...", 2 = "Unregister..."
    std::function<void()>               toggleReg;    // starts Register / Unregister (the tray posts it to itself: it may ask and show UAC)
};
void SetToolHooks(const ToolHooks& hooks);
// The worker changed state: refresh the queue view now (it also refreshes every 500 ms on its own). No-op when closed.
void NotifyQueueChanged();

// Control IDs, so tests can drive the window through SendMessage.
enum : int {
    ID_HOTKEY = 2001, ID_HOTKEY_PICK, ID_CODEC, ID_BITRATE, ID_MAXFPS, ID_OUTDIR, ID_BROWSE, ID_AUDIO, ID_AUDIODEV,
    ID_PROJECTION, ID_FILL, ID_FILLFADE, ID_SYMFOV, ID_SWAP, ID_ROLL, ID_SMOOTH, ID_SMOOTHVAL, ID_FOVSCALE,
    ID_INDICATOR, ID_INDSIZE, ID_INDOPACITY, ID_POSTMKV, ID_KEEPMP4, ID_POSTFLAT, ID_REQTRAY, ID_LOGLEVEL,
    ID_SAVE, ID_CANCEL, ID_OPENINI, ID_DEFAULTS,
    ID_HELPTEXT,  // the read-only "Description" panel (appended: existing ids do not move)
    ID_INDX, ID_INDY, ID_INDZ,  // indicator position (appended after ID_HELPTEXT for the same reason)
    ID_POSTYT8K,                // hidden check bound to post_yt8k (appended for the same reason)
    ID_YTMODE,                  // "YouTube output" drop-down: drives the hidden ID_POSTMKV / ID_POSTYT8K checks
    ID_AUTOSTART,               // "Start with Windows" check (HKCU Run key, not a config.ini key; appended: existing ids do not move)
    ID_LAYOUT3D,              // "Output layout" drop-down bound to layout_3d (appended: existing ids do not move)
    ID_HOTKEY_ADD,              // "Add...": captures a binding and appends it to the hotkey list (appended for the same reason)
    ID_POSTWHEN,                // "Convert" drop-down bound to post_when (appended for the same reason)
    ID_EYECROP,                 // "Eye crop" drop-down bound to eye_crop (appended for the same reason)
    ID_OVERLAYS,                // "Include overlay layers" check bound to record_overlays (appended for the same reason)
    ID_MAXWIDTH,                // "Resolution" drop-down bound to max_width (appended for the same reason)
    ID_INDPREVIEW,              // owner-drawn live preview of the indicator dot in the headset view (not a config key; appended for the same reason)
    ID_LAYOUTPREVIEW,           // owner-drawn live preview of the 3D layout: frame shapes and pixel sizes (not a config key; appended for the same reason)
    // Conversion controls (not config keys; appended for the same reason): "Convert a recording..." in the Export group and the
    // "Conversions" group (running job status + progress bar + Pause / Cancel; waiting list + Start now / Remove)
    ID_CONVERTPICK, ID_QSTATUS, ID_QPROGRESS, ID_QPAUSE, ID_QCANCEL, ID_QLIST, ID_QSTART, ID_QREMOVE,
    ID_BITRATEHINT,             // "about 54 GB per hour" next to the bitrate drop-down (not a config key)
    ID_YT8KBITRATE,             // "8K bitrate" drop-down bound to yt8k_bitrate_mbps
    ID_OUTFREE,                 // free space of the output folder's drive, under the folder field (not a config key)
    ID_YT8KCODEC,               // "8K codec" drop-down bound to yt8k_codec (appended: existing ids do not move)
    ID_ADDQUEUE,                // "Add to queue...": picked MP4s wait in the list with the window's format (not a config key)
    ID_QSTARTALL,               // "Start all": every waiting job (not a config key)
    ID_RECORDYT,                // "Record directly in the YouTube layout" check bound to record_for_youtube (appended: existing ids do not move)
    ID_LOGO,                    // owner-drawn logo (mark + wordmark) at the top of the window (not a config key; appended: existing ids do not move)
    ID_ABOUTLINK,               // SysLink in the footer: GitHub link + copyright / license (not a config key; appended: existing ids do not move)
    ID_BEEPS,                   // "Beep in the headset ..." check bound to beeps (appended: existing ids do not move)
    ID_RATECONTROL,             // "Rate control" drop-down bound to rate_control (appended for the same reason)
    ID_QP,                      // "QP" edit bound to qp; enabled only for rate_control = qp (appended for the same reason)
    ID_PRESET,                  // "Encoder preset" drop-down bound to quality_vs_speed (appended for the same reason)
    ID_CFR,                     // "Constant frame rate ..." check bound to constant_frame_rate (appended for the same reason)
    ID_FPSWARN                  // red warning line on the top edge of the Export group box: the game's frame rate is uneven for 60 fps YouTube video (hidden otherwise)
    , ID_OPENSTEAMVR            // footer button "SteamVR games...": opens the tray's SteamVR games window via SetOpenSteamVrHook (not a config key; appended: existing ids do not move)
    , ID_SVRMODE                // "SteamVR games:" drop-down in the Advanced group, bound to [steamvr] steamvr_mode (appended)
    , ID_MINIMIZE               // footer button "Minimize" (the window has no title bar; ID_CANCEL reads "Exit"; appended)
    , ID_OPENLOG, ID_TAG3D, ID_REGLAYER   // Advanced "Tools:" row (moved from the tray menu; SetToolHooks; appended)
    , ID_THEME                  // "Theme:" drop-down in the header, bound to [capture] theme = system|light|dark (appended)
};
// ID_QSTART reads "Start" and acts on every selected item of ID_QLIST (a multi-select list box: LB_SETSEL / LB_GETSELITEMS).
extern const wchar_t* const kWindowTitle;

} // namespace settings
