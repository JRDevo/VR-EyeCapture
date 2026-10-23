// install -- registers / unregisters the OpenXR implicit API layer for EyeCaptureTray.exe (pure registry logic, no UI).
//
// An OpenXR game loads the layer only if
//   HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit\<full path>\XR_APILAYER_JRDEVO_eye_capture.json = DWORD 0
// exists. ETS2 / ATS use their own loader that reads ONLY HKLM, so the layer is registered there and nowhere else: the
// same json also registered in HKCU makes the official loader load the layer twice (games crash). Rules (same as the
// developer script scripts\install.ps1):
//   - a value counts as "ours" when the LAST path component of its name equals the json name (case-insensitive);
//   - Register writes <exeDir>\json = 0 in HKLM and removes every other same-name value there (stale / moved folder);
//   - Unregister removes every same-name value from HKLM;
//   - HKCU duplicates are removed by the NON-elevated tray itself (RemoveHkcuDuplicates): the elevated process may run
//     as a different account, so its HKCU is not the user's.
// Writing HKLM needs administrator rights: the tray runs itself elevated with --register / --unregister.
// 32-bit OpenXR games (docs/X86_DESIGN.md): the 32-bit layer eye_capture32.dll + XR_APILAYER_JRDEVO_eye_capture32.json is
// registered by the same rules in the 32-bit registry view (KEY_WOW64_32KEY = HKLM\SOFTWARE\WOW6432Node\Khronos\...), which
// is what the 32-bit OpenXR loader reads; Register does it when those two files are next to the exe, Unregister always
// removes it, RemoveHkcuDuplicates removes HKCU copies of either json (HKCU\Software is the same key for both views).
//
// "Start with Windows" (optional, off by default) is a per-user registry state; the user's CHOICE is also the config.ini key
// start_with_windows (src/config.h), which lets the tray restore its own value at start (tray.cpp, see below):
//   HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "VR EyeCapture" (REG_SZ) = "<exeDir>\EyeCaptureTray.exe"
//   (quoted, no arguments). No admin rights needed. Before the rename (docs/RENAME_PLAN.md) the value was called
//   "OpenXR-EyeCapture". Behaviour rules (antivirus: a program that quietly rewrites its own startup entries looks like
//   malware, see docs/DEVELOPMENT.md "Antivirus: behaviour rules"):
//   - the Run value is written or deleted ONLY by SetAutostart: by the user changing the Settings check box, or by the tray at
//     start while start_with_windows = 1 and the value is missing / points at another folder (never when the user disabled
//     it in Task Manager). Without the key the start is read-only (ReadAutostart / AutostartStalePath);
//   - the StartupApproved record (HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run, same
//     value name, REG_BINARY, first byte even = enabled, odd = the user disabled it in Task Manager > Startup apps) is
//     Windows' own switch. It is only READ here (to show "disabled in Task Manager"), never written, moved or deleted;
//   - enabled = a Run value exists (new or old name) AND the record is missing or has an even first byte;
//   - SetAutostart(true) writes the "VR EyeCapture" value and deletes a value of the old name (user action);
//     SetAutostart(false) deletes both names. Neither touches the StartupApproved key;
//   - tests redirect the Run key to a subkey of HKCU with SetAutostartTestKey (the approval key becomes <subkey>"Approved"),
//     so a real Run / StartupApproved key is never touched.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace install {

enum class State {
    Registered,      // <exeDir>\json = 0 in HKLM, no other same-name value there, nothing in HKCU
    NotRegistered,   // no same-name value in HKLM (or ours exists but is not DWORD 0 = disabled)
    StalePath,       // a json with our file name is registered in HKLM from another folder (registeredPath)
    DuplicateHkcu    // registered correctly in HKLM, but a same-name value is also in HKCU
};

struct Status {
    // Combined: Registered / DuplicateHkcu only when the 64-bit layer is registered AND -- when its files are next to the
    // exe -- the 32-bit one too; otherwise the state of the part that is missing (registeredPath = that part's value).
    State state = State::NotRegistered;
    std::wstring registeredPath;   // Registered / DuplicateHkcu: ours; StalePath: the first value from another folder
    bool hkcuDuplicate = false;    // a same-name value (64- or 32-bit json) exists in HKCU (whatever the HKLM state)
    bool filesPresent = false;     // the json and eye_capture.dll are next to the exe
    // 32-bit OpenXR games (docs/X86_DESIGN.md): XR_APILAYER_JRDEVO_eye_capture32.json + eye_capture32.dll, registered in the
    // 32-bit registry view (HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit), the one the 32-bit loader reads.
    bool files32Present = false;
    State state64 = State::NotRegistered;   // Registered / NotRegistered / StalePath of the 64-bit json alone (no HKCU)
    State state32 = State::NotRegistered;   // Registered / NotRegistered / StalePath of the 32-bit json alone
    std::wstring registeredPath32;
};

// Read-only. exeDir = the folder of EyeCaptureTray.exe (where the json and eye_capture.dll must be).
Status Query(const std::wstring& exeDir);

// Writes HKLM (needs admin): adds <exeDir>\json = 0 and removes every other same-name value from HKLM. When nothing has
// to change it succeeds without opening the key for writing. False + err on failure; *winErr (optional) gets the
// Win32 error (ERROR_ACCESS_DENIED -> err says "needs administrator rights", ERROR_FILE_NOT_FOUND -> files missing).
// The same for the 32-bit layer in the 32-bit view when eye_capture32.dll + its json are next to the exe (skipped -- the
// 32-bit view untouched -- when they are not).
bool Register(const std::wstring& exeDir, std::wstring& err, long* winErr = nullptr);
// Removes every same-name value (64-bit json from the 64-bit view, 32-bit json from the 32-bit view) from HKLM (succeeds
// without admin when there is nothing to remove).
bool Unregister(const std::wstring& exeDir, std::wstring& err, long* winErr = nullptr);
// No admin: removes same-name values (both json names) from the CURRENT user's HKCU. Returns how many were removed.
int RemoveHkcuDuplicates();

// Read-only registry keys to watch (KEY_NOTIFY) for a change of the registration: the HKLM 64-bit / 32-bit view roots and the
// HKCU root -- or, for a root that does not exist yet, its nearest existing parent (subtree = true). The caller closes them.
struct WatchKey { HKEY key; bool subtree; };
std::vector<WatchKey> OpenWatchKeys();

// Tests: replace the two roots (default HKLM / HKCU \SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit) with keys given as
// "HKCU\\Software\\OpenXR-EyeCapture-Test\\LM" (prefix HKCU\ or HKLM\). Empty strings restore the defaults. The 32-bit
// view's HKLM root becomes <lmPath>32 (HKCU\Software is not redirected between the views, so it needs a key of its own).
void SetTestRoots(const std::wstring& lmPath, const std::wstring& cuPath);

// ---- Start with Windows (HKCU Run key, see the header comment) ----
const wchar_t* AutostartValueName();   // "VR EyeCapture"
// The command the Run value should hold for exeDir: "<exeDir>\EyeCaptureTray.exe" (quoted, no arguments).
std::wstring AutostartCommand(const std::wstring& exeDir);

// Read-only snapshot of the startup entry.
struct AutostartInfo {
    bool runPresent = false;                // value "VR EyeCapture" exists
    std::wstring runCommand;
    bool oldPresent = false;                // value "OpenXR-EyeCapture" (name before the rename) exists
    std::wstring oldCommand;
    bool disabledInTaskManager = false;     // the StartupApproved record of the present value has an odd first byte
};
AutostartInfo ReadAutostart();
// True when every present Run value (new / old name) starts the EyeCaptureTray.exe of exeDir (path compare, case-insensitive).
// False when none exists.
bool AutostartPointsHere(const AutostartInfo& info, const std::wstring& exeDir);
// "" when there is no Run value or it points at exeDir; else the program path the stale value points at (for a hint / balloon).
std::wstring AutostartStalePath(const std::wstring& exeDir);
// True when a Run value exists (any name, any data) AND its StartupApproved record is missing or enabled: a record the user
// disabled (Task Manager) returns false. *storedCommand (optional) gets the Run data (new name first), "" when there is none.
bool AutostartEnabled(std::wstring* storedCommand = nullptr);
// on: writes the "VR EyeCapture" value for exeDir and deletes an old-name value. off: deletes both names (missing = success).
// Never touches StartupApproved. Only call it from a user action or the start-up restore of start_with_windows = 1 (tray.cpp).
// False + err on a Win32 error.
bool SetAutostart(bool on, const std::wstring& exeDir, std::wstring& err);
// Tests: redirect the Run key to "Software\\OpenXR-EyeCapture-Test\\Run" under HKCU. Empty restores the real key.
void SetAutostartTestKey(const std::wstring& hkcuSubkey);

const wchar_t* JsonName();   // "XR_APILAYER_JRDEVO_eye_capture.json"
const wchar_t* DllName();    // "eye_capture.dll"
const wchar_t* JsonName32(); // "XR_APILAYER_JRDEVO_eye_capture32.json"
const wchar_t* DllName32();  // "eye_capture32.dll"
// The value name Register writes for the 32-bit layer: <full path of exeDir>\<32-bit json name>.
std::wstring OurValueName32(const std::wstring& exeDir);
// The value name Register writes for exeDir: <full path of exeDir>\<json name>.
std::wstring OurValueName(const std::wstring& exeDir);
// "registered" / "not registered" / "registered from another folder" / "registered (+ HKCU duplicate)".
const char* StateText(State s);

} // namespace install
