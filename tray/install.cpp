// install -- see install.h. Registry only; no UI, no logging (the callers log), so tests can drive it directly.
#include "install.h"

#include <windows.h>
#include <vector>

namespace install {
namespace {

const wchar_t kJsonName[] = L"XR_APILAYER_JRDEVO_eye_capture.json";
const wchar_t kDllName[] = L"eye_capture.dll";
// 32-bit OpenXR games (docs/X86_DESIGN.md): the 32-bit loader reads the 32-bit registry view (HKLM\SOFTWARE\WOW6432Node\...)
const wchar_t kJsonName32[] = L"XR_APILAYER_JRDEVO_eye_capture32.json";
const wchar_t kDllName32[] = L"eye_capture32.dll";
const wchar_t kImplicitKey[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
// 64-bit registry view explicitly: 64-bit games and their loader read the 64-bit view; the 32-bit layer goes to the 32-bit one.
const REGSAM kView = KEY_WOW64_64KEY;
const REGSAM kView32 = KEY_WOW64_32KEY;

struct Root {
    HKEY hive;
    std::wstring sub;
};
Root g_lm{HKEY_LOCAL_MACHINE, kImplicitKey};
Root g_lm32{HKEY_LOCAL_MACHINE, kImplicitKey};   // opened with kView32
Root g_cu{HKEY_CURRENT_USER, kImplicitKey};     // HKCU\Software is shared by both views: both json names are checked there

// One layer flavour: the 64-bit layer in the 64-bit view, the 32-bit layer in the 32-bit view.
struct Flavor {
    const wchar_t* json;
    const wchar_t* dll;
    const Root* lm;
    REGSAM view;
};
const Flavor kF64{kJsonName, kDllName, &g_lm, kView};
const Flavor kF32{kJsonName32, kDllName32, &g_lm32, kView32};

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"VR EyeCapture";
const wchar_t kOldRunValue[] = L"OpenXR-EyeCapture";   // the value name before the rename (read; replaced only by a user action in SetAutostart)
const wchar_t kTrayExe[] = L"EyeCaptureTray.exe";
const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
std::wstring g_runKey = kRunKey;   // HKCU subkey holding the Run value (tests redirect it)
std::wstring g_approvedKey = kApprovedKey;   // HKCU subkey holding the StartupApproved record (READ ONLY; tests redirect it too)

bool ParseRoot(const std::wstring& path, Root& out) {
    struct Prefix { const wchar_t* text; HKEY hive; };
    static const Prefix prefixes[] = {
        {L"HKCU\\", HKEY_CURRENT_USER}, {L"HKEY_CURRENT_USER\\", HKEY_CURRENT_USER},
        {L"HKLM\\", HKEY_LOCAL_MACHINE}, {L"HKEY_LOCAL_MACHINE\\", HKEY_LOCAL_MACHINE}};
    for (const Prefix& p : prefixes) {
        const size_t n = wcslen(p.text);
        if (path.size() > n && _wcsnicmp(path.c_str(), p.text, n) == 0) {
            out.hive = p.hive;
            out.sub = path.substr(n);
            return true;
        }
    }
    return false;
}

std::wstring RootText(const Root& r) {
    return std::wstring(r.hive == HKEY_LOCAL_MACHINE ? L"HKLM\\" : L"HKCU\\") + r.sub;
}

// Last path component (after the last '\' or '/'), like [System.IO.Path]::GetFileName in install.ps1.
std::wstring FileNameOf(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

bool IsOurName(const std::wstring& valueName, const wchar_t* json) { return _wcsicmp(FileNameOf(valueName).c_str(), json) == 0; }

// Full path, lower case: for comparing a registered value name with ours.
std::wstring Norm(const std::wstring& p) {
    std::wstring r = p;
    const DWORD need = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (need > 0) {
        std::vector<wchar_t> buf(need + 1);
        const DWORD n = GetFullPathNameW(p.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
        if (n > 0 && n < buf.size()) r.assign(buf.data(), n);
    }
    if (!r.empty()) CharLowerBuffW(&r[0], (DWORD)r.size());
    return r;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExeDirFull(const std::wstring& exeDir) {
    std::wstring d = exeDir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    // expand 8.3 short names (best effort), then make it absolute
    const DWORD need = GetLongPathNameW(d.c_str(), nullptr, 0);
    if (need > 0) {
        std::vector<wchar_t> buf(need + 1);
        const DWORD n = GetLongPathNameW(d.c_str(), buf.data(), (DWORD)buf.size());
        if (n > 0 && n < buf.size()) d.assign(buf.data(), n);
    }
    const DWORD needF = GetFullPathNameW(d.c_str(), 0, nullptr, nullptr);
    if (needF > 0) {
        std::vector<wchar_t> buf(needF + 1);
        const DWORD n = GetFullPathNameW(d.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
        if (n > 0 && n < buf.size()) d.assign(buf.data(), n);
    }
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
}

struct Entry {
    std::wstring name;
    DWORD type = REG_NONE;
    DWORD value = 0xFFFFFFFF;   // the DWORD when type == REG_DWORD
};

// Every value of the root whose file name is `json`. A missing key = empty list (ERROR_SUCCESS).
LSTATUS ListOurs(const Root& r, REGSAM view, const wchar_t* json, std::vector<Entry>& out) {
    out.clear();
    HKEY k = nullptr;
    LSTATUS e = RegOpenKeyExW(r.hive, r.sub.c_str(), 0, KEY_QUERY_VALUE | view, &k);
    if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (e != ERROR_SUCCESS) return e;
    DWORD count = 0, maxName = 0;
    e = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &maxName, nullptr, nullptr, nullptr);
    if (e == ERROR_SUCCESS) {
        std::vector<wchar_t> name(maxName + 2);
        for (DWORD i = 0; i < count; ++i) {
            DWORD len = (DWORD)name.size(), type = 0;
            BYTE data[sizeof(DWORD)] = {};
            DWORD size = sizeof(data);
            LSTATUS q = RegEnumValueW(k, i, name.data(), &len, nullptr, &type, data, &size);
            if (q == ERROR_MORE_DATA) {   // data bigger than a DWORD (not a DWORD value): name only
                len = (DWORD)name.size();
                size = 0;
                q = RegEnumValueW(k, i, name.data(), &len, nullptr, &type, nullptr, nullptr);
            }
            if (q == ERROR_NO_MORE_ITEMS) break;
            if (q != ERROR_SUCCESS) continue;
            Entry en;
            en.name.assign(name.data(), len);
            if (!IsOurName(en.name, json)) continue;
            en.type = type;
            if (type == REG_DWORD && size == sizeof(DWORD)) memcpy(&en.value, data, sizeof(DWORD));
            out.push_back(en);
        }
    }
    RegCloseKey(k);
    return e;
}

std::wstring Win32Text(LSTATUS e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   (DWORD)e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s + L" (error " + std::to_wstring(e) + L")";
}

bool Fail(const std::wstring& what, const Root& r, LSTATUS e, std::wstring& err, long* winErr, REGSAM view = kView) {
    if (winErr) *winErr = (long)e;
    const std::wstring where = RootText(r) + (view == kView32 ? L" (32-bit view)" : L"");
    if (e == ERROR_ACCESS_DENIED)
        err = what + L" " + where + L": needs administrator rights";
    else
        err = what + L" " + where + L": " + Win32Text(e);
    return false;
}

std::wstring ValueName(const std::wstring& exeDir, const wchar_t* json) { return ExeDirFull(exeDir) + L"\\" + json; }

// One flavour's HKLM state (Registered / NotRegistered / StalePath) and the registered path.
State QueryOne(const Flavor& f, const std::wstring& exeDir, std::wstring& path) {
    const std::wstring ours = ValueName(exeDir, f.json);
    const std::wstring oursNorm = Norm(ours);
    std::vector<Entry> lm;
    ListOurs(*f.lm, f.view, f.json, lm);   // unreadable = treated as empty
    bool haveOurs = false;
    std::wstring other;
    for (const Entry& e : lm) {
        if (Norm(e.name) == oursNorm) {
            if (e.type == REG_DWORD && e.value == 0) haveOurs = true;   // non-zero = disabled by the loader rules
        } else if (other.empty()) {
            other = e.name;
        }
    }
    path.clear();
    if (!other.empty()) { path = other; return State::StalePath; }
    if (haveOurs) { path = ours; return State::Registered; }
    return State::NotRegistered;
}

// <exeDir>\json = 0 in the flavour's HKLM view, every other same-name value there removed, verified.
bool RegisterOne(const Flavor& f, const std::wstring& exeDir, std::wstring& err, long* winErr) {
    const std::wstring ours = ValueName(exeDir, f.json);
    std::vector<Entry> lm;
    LSTATUS e = ListOurs(*f.lm, f.view, f.json, lm);
    if (e != ERROR_SUCCESS) return Fail(L"cannot read", *f.lm, e, err, winErr, f.view);
    std::vector<std::wstring> remove;
    bool haveOurs = false;
    for (const Entry& en : lm) {
        if (en.name == ours && en.type == REG_DWORD && en.value == 0) haveOurs = true;
        else if (en.name != ours) remove.push_back(en.name);   // stale folder, duplicate, or a case variant of ours
    }
    if (haveOurs && remove.empty()) return true;   // already exactly right: no write access needed
    HKEY k = nullptr;
    e = RegCreateKeyExW(f.lm->hive, f.lm->sub.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | f.view, nullptr, &k, nullptr);
    if (e != ERROR_SUCCESS) return Fail(L"cannot open", *f.lm, e, err, winErr, f.view);
    for (const std::wstring& n : remove) {
        e = RegDeleteValueW(k, n.c_str());
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) { RegCloseKey(k); return Fail(L"cannot remove " + n + L" from", *f.lm, e, err, winErr, f.view); }
    }
    const DWORD zero = 0;
    e = RegSetValueExW(k, ours.c_str(), 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
    RegCloseKey(k);
    if (e != ERROR_SUCCESS) return Fail(L"cannot write " + ours + L" to", *f.lm, e, err, winErr, f.view);
    // verify
    e = ListOurs(*f.lm, f.view, f.json, lm);
    if (e != ERROR_SUCCESS || lm.size() != 1 || lm[0].name != ours || lm[0].type != REG_DWORD || lm[0].value != 0) {
        if (winErr) *winErr = e != ERROR_SUCCESS ? (long)e : (long)ERROR_INVALID_DATA;
        err = L"verification failed: " + RootText(*f.lm) + (f.view == kView32 ? L" (32-bit view)" : L"") +
              L" does not hold exactly " + ours + L" = 0";
        return false;
    }
    return true;
}

// Every same-name value of the flavour removed from its HKLM view, verified.
bool UnregisterOne(const Flavor& f, std::wstring& err, long* winErr) {
    std::vector<Entry> lm;
    LSTATUS e = ListOurs(*f.lm, f.view, f.json, lm);
    if (e != ERROR_SUCCESS) return Fail(L"cannot read", *f.lm, e, err, winErr, f.view);
    if (lm.empty()) return true;
    HKEY k = nullptr;
    e = RegOpenKeyExW(f.lm->hive, f.lm->sub.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | f.view, &k);
    if (e != ERROR_SUCCESS) return Fail(L"cannot open", *f.lm, e, err, winErr, f.view);
    for (const Entry& en : lm) {
        e = RegDeleteValueW(k, en.name.c_str());
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) { RegCloseKey(k); return Fail(L"cannot remove " + en.name + L" from", *f.lm, e, err, winErr, f.view); }
    }
    RegCloseKey(k);
    e = ListOurs(*f.lm, f.view, f.json, lm);
    if (e != ERROR_SUCCESS || !lm.empty()) {
        if (winErr) *winErr = e != ERROR_SUCCESS ? (long)e : (long)ERROR_INVALID_DATA;
        err = L"verification failed: a " + std::wstring(f.json) + L" value is still in " + RootText(*f.lm) +
              (f.view == kView32 ? L" (32-bit view)" : L"");
        return false;
    }
    return true;
}

} // namespace

const wchar_t* JsonName() { return kJsonName; }
const wchar_t* DllName() { return kDllName; }
const wchar_t* JsonName32() { return kJsonName32; }
const wchar_t* DllName32() { return kDllName32; }

std::wstring OurValueName(const std::wstring& exeDir) { return ValueName(exeDir, kJsonName); }
std::wstring OurValueName32(const std::wstring& exeDir) { return ValueName(exeDir, kJsonName32); }

const char* StateText(State s) {
    switch (s) {
    case State::Registered: return "registered";
    case State::NotRegistered: return "not registered";
    case State::StalePath: return "registered from another folder";
    case State::DuplicateHkcu: return "registered (+ HKCU duplicate)";
    }
    return "?";
}

std::vector<WatchKey> OpenWatchKeys() {
    std::vector<WatchKey> out;
    const struct { const Root* r; REGSAM view; } roots[] = {{&g_lm, kView}, {&g_lm32, kView32}, {&g_cu, kView}};
    for (const auto& x : roots) {
        std::wstring sub = x.r->sub;
        bool subtree = false;
        for (;;) {
            HKEY k = nullptr;
            if (RegOpenKeyExW(x.r->hive, sub.c_str(), 0, KEY_NOTIFY | x.view, &k) == ERROR_SUCCESS) {
                out.push_back({k, subtree});
                break;
            }
            const size_t cut = sub.find_last_of(L'\\');
            if (cut == std::wstring::npos) break;
            sub.resize(cut);
            subtree = true;
        }
    }
    return out;
}

void SetTestRoots(const std::wstring& lmPath, const std::wstring& cuPath) {
    Root lm{HKEY_LOCAL_MACHINE, kImplicitKey}, lm32{HKEY_LOCAL_MACHINE, kImplicitKey}, cu{HKEY_CURRENT_USER, kImplicitKey};
    if (!lmPath.empty()) { ParseRoot(lmPath, lm); ParseRoot(lmPath + L"32", lm32); }
    if (!cuPath.empty()) ParseRoot(cuPath, cu);
    g_lm = lm;
    g_lm32 = lm32;
    g_cu = cu;
}

Status Query(const std::wstring& exeDir) {
    Status s;
    const std::wstring dir = ExeDirFull(exeDir);
    s.filesPresent = FileExists(dir + L"\\" + kJsonName) && FileExists(dir + L"\\" + kDllName);
    s.files32Present = FileExists(dir + L"\\" + kJsonName32) && FileExists(dir + L"\\" + kDllName32);
    s.state = s.state64 = QueryOne(kF64, exeDir, s.registeredPath);
    s.state32 = QueryOne(kF32, exeDir, s.registeredPath32);
    std::vector<Entry> cu, cu32;
    ListOurs(g_cu, kView, kJsonName, cu);
    ListOurs(g_cu, kView, kJsonName32, cu32);
    s.hkcuDuplicate = !cu.empty() || !cu32.empty();
    // combined: with the 32-bit files here, "registered" means both layers
    if (s.state == State::Registered && s.files32Present && s.state32 != State::Registered) {
        s.state = s.state32;
        s.registeredPath = s.registeredPath32;
    }
    if (s.state == State::Registered && s.hkcuDuplicate) s.state = State::DuplicateHkcu;
    return s;
}

bool Register(const std::wstring& exeDir, std::wstring& err, long* winErr) {
    err.clear();
    if (winErr) *winErr = 0;
    const std::wstring dir = ExeDirFull(exeDir);
    if (!FileExists(dir + L"\\" + kJsonName) || !FileExists(dir + L"\\" + kDllName)) {
        if (winErr) *winErr = ERROR_FILE_NOT_FOUND;
        err = std::wstring(kDllName) + L" / " + kJsonName + L" not found next to EyeCaptureTray.exe (" + dir + L")";
        return false;
    }
    if (!RegisterOne(kF64, exeDir, err, winErr)) return false;
    // 32-bit games: only when the 32-bit layer ships next to the exe (a developer build without build32 leaves that view alone)
    if (FileExists(dir + L"\\" + kJsonName32) && FileExists(dir + L"\\" + kDllName32) && !RegisterOne(kF32, exeDir, err, winErr))
        return false;
    return true;
}

bool Unregister(const std::wstring& exeDir, std::wstring& err, long* winErr) {
    (void)exeDir;   // every same-name value goes, whichever folder it points at
    err.clear();
    if (winErr) *winErr = 0;
    std::wstring err32;
    long win32 = 0;
    const bool ok64 = UnregisterOne(kF64, err, winErr);
    const bool ok32 = UnregisterOne(kF32, err32, &win32);   // also when the 64-bit part failed: as much as possible goes
    if (!ok32) {
        if (ok64 && winErr) *winErr = win32;
        err = err.empty() ? err32 : err + L"; " + err32;
    }
    return ok64 && ok32;
}

const wchar_t* AutostartValueName() { return kRunValue; }

std::wstring AutostartCommand(const std::wstring& exeDir) {
    return L"\"" + ExeDirFull(exeDir) + L"\\" + kTrayExe + L"\"";
}

void SetAutostartTestKey(const std::wstring& hkcuSubkey) {
    g_runKey = hkcuSubkey.empty() ? std::wstring(kRunKey) : hkcuSubkey;
    g_approvedKey = hkcuSubkey.empty() ? std::wstring(kApprovedKey) : hkcuSubkey + L"Approved";
}

namespace {
enum class Approval { Missing, Enabled, Disabled };

// StartupApproved\Run record (READ ONLY -- this module never writes, moves or deletes it; Windows owns it):
// REG_BINARY, first byte even = enabled, odd = disabled by the user (Task Manager / Settings).
Approval ReadApproval(const wchar_t* valueName) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, g_approvedKey.c_str(), 0, KEY_QUERY_VALUE | kView, &k) != ERROR_SUCCESS) return Approval::Missing;
    BYTE data[16] = {};
    DWORD type = 0, size = sizeof(data);
    const LSTATUS e = RegQueryValueExW(k, valueName, nullptr, &type, data, &size);
    RegCloseKey(k);
    if (e == ERROR_MORE_DATA) return Approval::Enabled;   // longer than a normal record: leave it alone
    if (e != ERROR_SUCCESS || type != REG_BINARY || size < 1) return Approval::Missing;
    return (data[0] & 1) ? Approval::Disabled : Approval::Enabled;
}

// Reads one Run value (any type); false = no key / no value. *text gets the REG_SZ / REG_EXPAND_SZ data.
bool ReadRunValue(const wchar_t* name, std::wstring* text) {
    if (text) text->clear();
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, g_runKey.c_str(), 0, KEY_QUERY_VALUE | kView, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    const bool ok = RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    if (ok && text && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
        DWORD sz2 = (DWORD)(buf.size() * sizeof(wchar_t)) - sizeof(wchar_t);
        if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf.data(), &sz2) == ERROR_SUCCESS) *text = buf.data();
    }
    RegCloseKey(k);
    return ok;
}

// The program path inside a Run command: "C:\x y\a.exe" (quoted) or C:\x\a.exe [args].
std::wstring CommandExe(const std::wstring& cmd) {
    const size_t b = cmd.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return L"";
    if (cmd[b] == L'"') {
        const size_t e = cmd.find(L'"', b + 1);
        return cmd.substr(b + 1, e == std::wstring::npos ? std::wstring::npos : e - b - 1);
    }
    const size_t e = cmd.find(L' ', b);
    return cmd.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
}
} // namespace

AutostartInfo ReadAutostart() {
    AutostartInfo i;
    i.runPresent = ReadRunValue(kRunValue, &i.runCommand);
    i.oldPresent = ReadRunValue(kOldRunValue, &i.oldCommand);
    const wchar_t* shown = i.runPresent ? kRunValue : (i.oldPresent ? kOldRunValue : nullptr);
    i.disabledInTaskManager = shown && ReadApproval(shown) == Approval::Disabled;
    return i;
}

bool AutostartEnabled(std::wstring* storedCommand) {
    const AutostartInfo i = ReadAutostart();
    if (storedCommand) *storedCommand = i.runPresent ? i.runCommand : i.oldCommand;
    return (i.runPresent || i.oldPresent) && !i.disabledInTaskManager;
}

bool AutostartPointsHere(const AutostartInfo& i, const std::wstring& exeDir) {
    if (!i.runPresent && !i.oldPresent) return false;
    const std::wstring want = Norm(CommandExe(AutostartCommand(exeDir)));
    if (i.runPresent && Norm(CommandExe(i.runCommand)) != want) return false;
    if (i.oldPresent && Norm(CommandExe(i.oldCommand)) != want) return false;
    return true;
}

std::wstring AutostartStalePath(const std::wstring& exeDir) {
    const AutostartInfo i = ReadAutostart();
    if ((!i.runPresent && !i.oldPresent) || AutostartPointsHere(i, exeDir)) return L"";
    const std::wstring want = Norm(CommandExe(AutostartCommand(exeDir)));
    if (i.runPresent && Norm(CommandExe(i.runCommand)) != want) return CommandExe(i.runCommand);
    return CommandExe(i.oldCommand);
}

bool SetAutostart(bool on, const std::wstring& exeDir, std::wstring& err) {
    err.clear();
    const std::wstring what = L"HKCU\\" + g_runKey;
    if (on) {
        HKEY k = nullptr;
        LSTATUS e = RegCreateKeyExW(HKEY_CURRENT_USER, g_runKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE | kView, nullptr,
                                    &k, nullptr);
        if (e != ERROR_SUCCESS) { err = L"cannot open " + what + L": " + Win32Text(e); return false; }
        const std::wstring cmd = AutostartCommand(exeDir);
        e = RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        if (e != ERROR_SUCCESS) {
            RegCloseKey(k);
            err = L"cannot write " + std::wstring(kRunValue) + L" to " + what + L": " + Win32Text(e);
            return false;
        }
        // this is a user action: the entry of the old name (before the rename) is replaced by the new one
        RegDeleteValueW(k, kOldRunValue);
        RegCloseKey(k);
        return true;
    }
    HKEY k = nullptr;
    LSTATUS e = RegOpenKeyExW(HKEY_CURRENT_USER, g_runKey.c_str(), 0, KEY_SET_VALUE | kView, &k);
    if (e == ERROR_FILE_NOT_FOUND) return true;   // no key = no value
    if (e != ERROR_SUCCESS) { err = L"cannot open " + what + L": " + Win32Text(e); return false; }
    for (const wchar_t* n : {kRunValue, kOldRunValue}) {
        e = RegDeleteValueW(k, n);
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) {
            RegCloseKey(k);
            err = L"cannot remove " + std::wstring(n) + L" from " + what + L": " + Win32Text(e);
            return false;
        }
    }
    RegCloseKey(k);
    return true;
}

int RemoveHkcuDuplicates() {
    std::vector<Entry> cu, cu32;
    if (ListOurs(g_cu, kView, kJsonName, cu) != ERROR_SUCCESS) cu.clear();
    if (ListOurs(g_cu, kView, kJsonName32, cu32) != ERROR_SUCCESS) cu32.clear();
    cu.insert(cu.end(), cu32.begin(), cu32.end());
    if (cu.empty()) return 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(g_cu.hive, g_cu.sub.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | kView, &k) != ERROR_SUCCESS) return 0;
    int removed = 0;
    for (const Entry& en : cu)
        if (RegDeleteValueW(k, en.name.c_str()) == ERROR_SUCCESS) ++removed;
    RegCloseKey(k);
    return removed;
}

} // namespace install
