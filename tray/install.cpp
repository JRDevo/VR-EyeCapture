// install -- see install.h. Registry only; no UI, no logging (the callers log), so tests can drive it directly.
#include "install.h"

#include <windows.h>
#include <vector>

namespace install {
namespace {

const wchar_t kJsonName[] = L"XR_APILAYER_JRDEVO_eye_capture.json";
const wchar_t kDllName[] = L"eye_capture.dll";
const wchar_t kImplicitKey[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
// 64-bit registry view explicitly: the games are 64-bit and their loader reads the 64-bit view.
const REGSAM kView = KEY_WOW64_64KEY;

struct Root {
    HKEY hive;
    std::wstring sub;
};
Root g_lm{HKEY_LOCAL_MACHINE, kImplicitKey};
Root g_cu{HKEY_CURRENT_USER, kImplicitKey};

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"OpenXR-EyeCapture";
const wchar_t kTrayExe[] = L"EyeCaptureTray.exe";
const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
std::wstring g_runKey = kRunKey;   // HKCU subkey holding the Run value (tests redirect it)
std::wstring g_approvedKey = kApprovedKey;   // HKCU subkey holding the StartupApproved record (tests redirect it too)

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

bool IsOurName(const std::wstring& valueName) { return _wcsicmp(FileNameOf(valueName).c_str(), kJsonName) == 0; }

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

// Every value of the root whose file name is ours. A missing key = empty list (ERROR_SUCCESS).
LSTATUS ListOurs(const Root& r, std::vector<Entry>& out) {
    out.clear();
    HKEY k = nullptr;
    LSTATUS e = RegOpenKeyExW(r.hive, r.sub.c_str(), 0, KEY_QUERY_VALUE | kView, &k);
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
            if (!IsOurName(en.name)) continue;
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

bool Fail(const std::wstring& what, const Root& r, LSTATUS e, std::wstring& err, long* winErr) {
    if (winErr) *winErr = (long)e;
    if (e == ERROR_ACCESS_DENIED)
        err = what + L" " + RootText(r) + L": needs administrator rights";
    else
        err = what + L" " + RootText(r) + L": " + Win32Text(e);
    return false;
}

} // namespace

const wchar_t* JsonName() { return kJsonName; }
const wchar_t* DllName() { return kDllName; }

std::wstring OurValueName(const std::wstring& exeDir) { return ExeDirFull(exeDir) + L"\\" + kJsonName; }

const char* StateText(State s) {
    switch (s) {
    case State::Registered: return "registered";
    case State::NotRegistered: return "not registered";
    case State::StalePath: return "registered from another folder";
    case State::DuplicateHkcu: return "registered (+ HKCU duplicate)";
    }
    return "?";
}

void SetTestRoots(const std::wstring& lmPath, const std::wstring& cuPath) {
    Root lm{HKEY_LOCAL_MACHINE, kImplicitKey}, cu{HKEY_CURRENT_USER, kImplicitKey};
    if (!lmPath.empty()) ParseRoot(lmPath, lm);
    if (!cuPath.empty()) ParseRoot(cuPath, cu);
    g_lm = lm;
    g_cu = cu;
}

Status Query(const std::wstring& exeDir) {
    Status s;
    const std::wstring dir = ExeDirFull(exeDir);
    s.filesPresent = FileExists(dir + L"\\" + kJsonName) && FileExists(dir + L"\\" + kDllName);
    const std::wstring ours = OurValueName(exeDir);
    const std::wstring oursNorm = Norm(ours);
    std::vector<Entry> lm, cu;
    ListOurs(g_lm, lm);   // unreadable = treated as empty
    bool haveOurs = false;
    std::wstring other;
    for (const Entry& e : lm) {
        if (Norm(e.name) == oursNorm) {
            if (e.type == REG_DWORD && e.value == 0) haveOurs = true;   // non-zero = disabled by the loader rules
        } else if (other.empty()) {
            other = e.name;
        }
    }
    if (!other.empty()) {
        s.state = State::StalePath;
        s.registeredPath = other;
    } else if (haveOurs) {
        s.state = State::Registered;
        s.registeredPath = ours;
    }
    ListOurs(g_cu, cu);
    s.hkcuDuplicate = !cu.empty();
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
    const std::wstring ours = OurValueName(exeDir);
    std::vector<Entry> lm;
    LSTATUS e = ListOurs(g_lm, lm);
    if (e != ERROR_SUCCESS) return Fail(L"cannot read", g_lm, e, err, winErr);
    std::vector<std::wstring> remove;
    bool haveOurs = false;
    for (const Entry& en : lm) {
        if (en.name == ours && en.type == REG_DWORD && en.value == 0) haveOurs = true;
        else if (en.name != ours) remove.push_back(en.name);   // stale folder, duplicate, or a case variant of ours
    }
    if (haveOurs && remove.empty()) return true;   // already exactly right: no write access needed
    HKEY k = nullptr;
    e = RegCreateKeyExW(g_lm.hive, g_lm.sub.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | kView, nullptr, &k, nullptr);
    if (e != ERROR_SUCCESS) return Fail(L"cannot open", g_lm, e, err, winErr);
    for (const std::wstring& n : remove) {
        e = RegDeleteValueW(k, n.c_str());
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) { RegCloseKey(k); return Fail(L"cannot remove " + n + L" from", g_lm, e, err, winErr); }
    }
    const DWORD zero = 0;
    e = RegSetValueExW(k, ours.c_str(), 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
    RegCloseKey(k);
    if (e != ERROR_SUCCESS) return Fail(L"cannot write " + ours + L" to", g_lm, e, err, winErr);
    // verify
    e = ListOurs(g_lm, lm);
    if (e != ERROR_SUCCESS || lm.size() != 1 || lm[0].name != ours || lm[0].type != REG_DWORD || lm[0].value != 0) {
        if (winErr) *winErr = e != ERROR_SUCCESS ? (long)e : (long)ERROR_INVALID_DATA;
        err = L"verification failed: " + RootText(g_lm) + L" does not hold exactly " + ours + L" = 0";
        return false;
    }
    return true;
}

bool Unregister(const std::wstring& exeDir, std::wstring& err, long* winErr) {
    (void)exeDir;   // every same-name value goes, whichever folder it points at
    err.clear();
    if (winErr) *winErr = 0;
    std::vector<Entry> lm;
    LSTATUS e = ListOurs(g_lm, lm);
    if (e != ERROR_SUCCESS) return Fail(L"cannot read", g_lm, e, err, winErr);
    if (lm.empty()) return true;
    HKEY k = nullptr;
    e = RegOpenKeyExW(g_lm.hive, g_lm.sub.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | kView, &k);
    if (e != ERROR_SUCCESS) return Fail(L"cannot open", g_lm, e, err, winErr);
    for (const Entry& en : lm) {
        e = RegDeleteValueW(k, en.name.c_str());
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) { RegCloseKey(k); return Fail(L"cannot remove " + en.name + L" from", g_lm, e, err, winErr); }
    }
    RegCloseKey(k);
    e = ListOurs(g_lm, lm);
    if (e != ERROR_SUCCESS || !lm.empty()) {
        if (winErr) *winErr = e != ERROR_SUCCESS ? (long)e : (long)ERROR_INVALID_DATA;
        err = L"verification failed: a " + std::wstring(kJsonName) + L" value is still in " + RootText(g_lm);
        return false;
    }
    return true;
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

// StartupApproved\Run record: REG_BINARY, first byte even = enabled, odd = disabled by the user (Task Manager / Settings).
Approval ReadApproval() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, g_approvedKey.c_str(), 0, KEY_QUERY_VALUE | kView, &k) != ERROR_SUCCESS) return Approval::Missing;
    BYTE data[16] = {};
    DWORD type = 0, size = sizeof(data);
    const LSTATUS e = RegQueryValueExW(k, kRunValue, nullptr, &type, data, &size);
    RegCloseKey(k);
    if (e == ERROR_MORE_DATA) return Approval::Enabled;   // longer than a normal record: leave it alone
    if (e != ERROR_SUCCESS || type != REG_BINARY || size < 1) return Approval::Missing;
    return (data[0] & 1) ? Approval::Disabled : Approval::Enabled;
}

bool WriteApproval(std::wstring& err) {
    const std::wstring what = L"HKCU\\" + g_approvedKey;
    HKEY k = nullptr;
    LSTATUS e = RegCreateKeyExW(HKEY_CURRENT_USER, g_approvedKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE | kView, nullptr, &k, nullptr);
    if (e != ERROR_SUCCESS) { err = L"cannot open " + what + L": " + Win32Text(e); return false; }
    const BYTE rec[12] = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};   // enabled, no disable timestamp
    e = RegSetValueExW(k, kRunValue, 0, REG_BINARY, rec, sizeof(rec));
    RegCloseKey(k);
    if (e != ERROR_SUCCESS) { err = L"cannot write " + std::wstring(kRunValue) + L" to " + what + L": " + Win32Text(e); return false; }
    return true;
}

bool DeleteApproval(std::wstring& err) {
    const std::wstring what = L"HKCU\\" + g_approvedKey;
    HKEY k = nullptr;
    LSTATUS e = RegOpenKeyExW(HKEY_CURRENT_USER, g_approvedKey.c_str(), 0, KEY_SET_VALUE | kView, &k);
    if (e == ERROR_FILE_NOT_FOUND) return true;
    if (e != ERROR_SUCCESS) { err = L"cannot open " + what + L": " + Win32Text(e); return false; }
    e = RegDeleteValueW(k, kRunValue);
    RegCloseKey(k);
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) { err = L"cannot remove " + std::wstring(kRunValue) + L" from " + what + L": " + Win32Text(e); return false; }
    return true;
}
} // namespace

bool AutostartEnabled(std::wstring* storedCommand) {
    if (storedCommand) storedCommand->clear();
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, g_runKey.c_str(), 0, KEY_QUERY_VALUE | kView, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    LSTATUS e = RegQueryValueExW(k, kRunValue, nullptr, &type, nullptr, &size);
    if (e != ERROR_SUCCESS) { RegCloseKey(k); return false; }
    if (storedCommand && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
        DWORD sz2 = (DWORD)(buf.size() * sizeof(wchar_t)) - sizeof(wchar_t);
        if (RegQueryValueExW(k, kRunValue, nullptr, &type, (LPBYTE)buf.data(), &sz2) == ERROR_SUCCESS) *storedCommand = buf.data();
    }
    RegCloseKey(k);
    return ReadApproval() != Approval::Disabled;
}

bool SetAutostart(bool on, const std::wstring& exeDir, std::wstring& err) {
    err.clear();
    const std::wstring what = L"HKCU\\" + g_runKey;
    if (on) {
        HKEY k = nullptr;
        LSTATUS e = RegCreateKeyExW(HKEY_CURRENT_USER, g_runKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE | kView, nullptr,
                                    &k, nullptr);
        if (e != ERROR_SUCCESS) { err = L"cannot open " + what + L": " + Win32Text(e); return false; }
        // the value as it was: put back when the approval record cannot be written (all or nothing)
        DWORD oldType = REG_NONE, oldSize = 0;
        std::vector<BYTE> old;
        bool hadOld = RegQueryValueExW(k, kRunValue, nullptr, &oldType, nullptr, &oldSize) == ERROR_SUCCESS;
        if (hadOld) {
            old.resize(oldSize ? oldSize : 1);
            hadOld = RegQueryValueExW(k, kRunValue, nullptr, &oldType, old.data(), &oldSize) == ERROR_SUCCESS;
        }
        const std::wstring cmd = AutostartCommand(exeDir);
        e = RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        if (e != ERROR_SUCCESS) {
            RegCloseKey(k);
            err = L"cannot write " + std::wstring(kRunValue) + L" to " + what + L": " + Win32Text(e);
            return false;
        }
        const bool ok = WriteApproval(err);
        if (!ok) {   // a Run value without its record would start (or show checked) although the call reports failure
            if (hadOld) RegSetValueExW(k, kRunValue, 0, oldType, old.data(), oldSize);
            else RegDeleteValueW(k, kRunValue);
        }
        RegCloseKey(k);
        return ok;
    }
    HKEY k = nullptr;
    LSTATUS e = RegOpenKeyExW(HKEY_CURRENT_USER, g_runKey.c_str(), 0, KEY_SET_VALUE | kView, &k);
    if (e != ERROR_FILE_NOT_FOUND) {   // no key = no value
        if (e != ERROR_SUCCESS) { err = L"cannot open " + what + L": " + Win32Text(e); return false; }
        e = RegDeleteValueW(k, kRunValue);
        RegCloseKey(k);
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) { err = L"cannot remove " + std::wstring(kRunValue) + L" from " + what + L": " + Win32Text(e); return false; }
    }
    return DeleteApproval(err);
}

bool SyncAutostart(const std::wstring& exeDir) {
    std::wstring stored;
    if (!AutostartEnabled(&stored)) return false;
    const std::wstring want = AutostartCommand(exeDir);
    if (_wcsicmp(stored.c_str(), want.c_str()) == 0 && ReadApproval() != Approval::Missing) return false;
    std::wstring err;
    return SetAutostart(true, exeDir, err);
}

int RemoveHkcuDuplicates() {
    std::vector<Entry> cu;
    if (ListOurs(g_cu, cu) != ERROR_SUCCESS || cu.empty()) return 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(g_cu.hive, g_cu.sub.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | kView, &k) != ERROR_SUCCESS) return 0;
    int removed = 0;
    for (const Entry& en : cu)
        if (RegDeleteValueW(k, en.name.c_str()) == ERROR_SUCCESS) ++removed;
    RegCloseKey(k);
    return removed;
}

} // namespace install
