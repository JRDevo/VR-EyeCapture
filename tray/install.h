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
//
// "Start with Windows" (optional, off by default) is a separate, per-user registry state, NOT a config.ini key:
//   HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "VR EyeCapture" (REG_SZ) = "<exeDir>\EyeCaptureTray.exe"
//   (quoted, no arguments). No admin rights needed. Before the rename (docs/RENAME_PLAN.md) the value was called
//   "OpenXR-EyeCapture"; MigrateAutostartName moves it (and its StartupApproved record) at tray start. Rules:
//   - newer Windows 11 builds start a Run entry at sign-in only when it also has an enabled approval record:
//     HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run, same value name, REG_BINARY of 12 bytes,
//     first byte even (2) = enabled, odd (3) = disabled by the user in Task Manager / Settings. SetAutostart(true) writes
//     02 00 00 00 + 8 zero bytes, SetAutostart(false) deletes it;
//   - enabled = the Run value exists (whatever its data) AND the approval record is missing or has an even first byte;
//     a record the user disabled reads as off (the Settings checkbox shows unchecked; checking it again rewrites 02);
//     SetAutostart(true) writes the Run value, then the record (all or nothing: when the record cannot be written the Run
//     value is put back as it was), SetAutostart(false) deletes both (a missing value counts as success);
//   - SyncAutostart rewrites an enabled entry whose data is not the expected command for this exe folder (the folder
//     was moved) or that has no approval record yet (an older install); it never turns the option on by itself;
//   - tests redirect the Run key to a subkey of HKCU with SetAutostartTestKey (the approval key becomes <subkey>"Approved"),
//     so a real Run / StartupApproved key is never touched.
#pragma once
#include <string>

namespace install {

enum class State {
    Registered,      // <exeDir>\json = 0 in HKLM, no other same-name value there, nothing in HKCU
    NotRegistered,   // no same-name value in HKLM (or ours exists but is not DWORD 0 = disabled)
    StalePath,       // a json with our file name is registered in HKLM from another folder (registeredPath)
    DuplicateHkcu    // registered correctly in HKLM, but a same-name value is also in HKCU
};

struct Status {
    State state = State::NotRegistered;
    std::wstring registeredPath;   // Registered / DuplicateHkcu: ours; StalePath: the first value from another folder
    bool hkcuDuplicate = false;    // a same-name value exists in HKCU (whatever the HKLM state)
    bool filesPresent = false;     // the json and eye_capture.dll are next to the exe
};

// Read-only. exeDir = the folder of EyeCaptureTray.exe (where the json and eye_capture.dll must be).
Status Query(const std::wstring& exeDir);

// Writes HKLM (needs admin): adds <exeDir>\json = 0 and removes every other same-name value from HKLM. When nothing has
// to change it succeeds without opening the key for writing. False + err on failure; *winErr (optional) gets the
// Win32 error (ERROR_ACCESS_DENIED -> err says "needs administrator rights", ERROR_FILE_NOT_FOUND -> files missing).
bool Register(const std::wstring& exeDir, std::wstring& err, long* winErr = nullptr);
// Removes every same-name value from HKLM (succeeds without admin when there is nothing to remove).
bool Unregister(const std::wstring& exeDir, std::wstring& err, long* winErr = nullptr);
// No admin: removes same-name values from the CURRENT user's HKCU. Returns how many were removed.
int RemoveHkcuDuplicates();

// Tests: replace the two roots (default HKLM / HKCU \SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit) with keys given as
// "HKCU\\Software\\OpenXR-EyeCapture-Test\\LM" (prefix HKCU\ or HKLM\). Empty strings restore the defaults.
void SetTestRoots(const std::wstring& lmPath, const std::wstring& cuPath);

// ---- Start with Windows (HKCU Run key, see the header comment) ----
const wchar_t* AutostartValueName();   // "VR EyeCapture"
// The rename: a Run value "OpenXR-EyeCapture" becomes "VR EyeCapture" with the same type and data (an existing new value is
// kept as it is), then the old one is deleted; the same for its StartupApproved record. Returns true when something moved
// (*what, optional: a line for the log). No-op (false) when there is no old value.
bool MigrateAutostartName(std::wstring* what = nullptr);
// The command the Run value should hold for exeDir: "<exeDir>\EyeCaptureTray.exe" (quoted, no arguments).
std::wstring AutostartCommand(const std::wstring& exeDir);
// True when the Run value exists (any data) AND its StartupApproved record is missing or enabled: a record the user disabled
// (Task Manager / Settings) returns false. *storedCommand (optional) gets the Run value's data whenever the value exists,
// also when the user disabled it; it is empty only when there is no Run value.
bool AutostartEnabled(std::wstring* storedCommand = nullptr);
// on: writes the Run value for exeDir and an enabled StartupApproved record; all or nothing: when the record cannot be
// written, the Run value is put back as it was (deleted again if it did not exist) and false is returned.
// off: deletes both (already missing = success). False + err on a Win32 error.
bool SetAutostart(bool on, const std::wstring& exeDir, std::wstring& err);
// If enabled and the stored command differs from AutostartCommand(exeDir) (case-insensitive), rewrites it. Returns true
// only when it rewrote the value; a no-op (false) when not enabled or already right.
bool SyncAutostart(const std::wstring& exeDir);
// Tests: redirect the Run key to "Software\\OpenXR-EyeCapture-Test\\Run" under HKCU. Empty restores the real key.
void SetAutostartTestKey(const std::wstring& hkcuSubkey);

const wchar_t* JsonName();   // "XR_APILAYER_JRDEVO_eye_capture.json"
const wchar_t* DllName();    // "eye_capture.dll"
// The value name Register writes for exeDir: <full path of exeDir>\<json name>.
std::wstring OurValueName(const std::wstring& exeDir);
// "registered" / "not registered" / "registered from another folder" / "registered (+ HKCU duplicate)".
const char* StateText(State s);

} // namespace install
