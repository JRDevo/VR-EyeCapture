// install_test -- unit test for tray/install.cpp (layer registration). No admin rights, no real HKLM / HKCU OpenXR key:
// both roots are redirected with install::SetTestRoots to keys under HKCU\Software\OpenXR-EyeCapture-Test, which is
// deleted at the start and at the end.
//
//   install_test             all cases (exit code 0 = passed)
//   install_test lockdemo    leaves HKCU\Software\OpenXR-EyeCapture-Test\RO: a read-only key (protected DACL, read for
//                            everyone) holding a stale same-name value, plus empty ...\CU. tests\run_install.ps1 points
//                            EyeCaptureTray.exe --register at it (EYECAP_TRAY_TEST_REG_ROOTS) and expects exit code 5.
//   install_test cleanup     restores the DACL of ...\RO and deletes HKCU\Software\OpenXR-EyeCapture-Test.
#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "config.h"
#include "install.h"
#include "log.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
std::string N(const std::wstring& w) { return elog::Narrow(w); }
std::string ReadText(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const wchar_t kBase[] = L"Software\\OpenXR-EyeCapture-Test";
const wchar_t kLm[] = L"Software\\OpenXR-EyeCapture-Test\\LM";
const wchar_t kLm32[] = L"Software\\OpenXR-EyeCapture-Test\\LM32";   // the 32-bit view's root that SetTestRoots(kLm, ...) derives
const wchar_t kCu[] = L"Software\\OpenXR-EyeCapture-Test\\CU";
const wchar_t kRo[] = L"Software\\OpenXR-EyeCapture-Test\\RO";
const wchar_t kUnrelated[] = L"C:\\Program Files\\Other Vendor\\other_layer.json";
const wchar_t kLookalike[] = L"C:\\Somewhere\\XR_APILAYER_JRDEVO_eye_capture.json.bak";

struct Val { std::wstring name; DWORD type; DWORD value; };

std::vector<Val> Values(const wchar_t* sub) {
    std::vector<Val> out;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[2048];
        DWORD len = 2048, type = 0, v = 0xFFFFFFFF, size = sizeof(v);
        const LSTATUS e = RegEnumValueW(k, i, name, &len, nullptr, &type, (BYTE*)&v, &size);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS) continue;
        out.push_back({std::wstring(name, len), type, v});
    }
    RegCloseKey(k);
    return out;
}

bool SetDword(const wchar_t* sub, const std::wstring& name, DWORD v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LSTATUS e = RegSetValueExW(k, name.c_str(), 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
    return e == ERROR_SUCCESS;
}

int CountSameName(const std::vector<Val>& vs) {
    int n = 0;
    for (const Val& v : vs) {
        const size_t k = v.name.find_last_of(L"\\/");
        const std::wstring f = k == std::wstring::npos ? v.name : v.name.substr(k + 1);
        if (_wcsicmp(f.c_str(), install::JsonName()) == 0) ++n;
    }
    return n;
}
int CountSameName32(const std::vector<Val>& vs) {
    int n = 0;
    for (const Val& v : vs) {
        const size_t k = v.name.find_last_of(L"\\/");
        const std::wstring f = k == std::wstring::npos ? v.name : v.name.substr(k + 1);
        if (_wcsicmp(f.c_str(), install::JsonName32()) == 0) ++n;
    }
    return n;
}
bool Has(const std::vector<Val>& vs, const std::wstring& name) {
    for (const Val& v : vs) if (v.name == name) return true;
    return false;
}

bool SetSddl(const wchar_t* sub, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    HKEY k = nullptr;
    LSTATUS e = RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, WRITE_DAC | READ_CONTROL, &k);
    if (e == ERROR_SUCCESS) {
        e = RegSetKeySecurity(k, DACL_SECURITY_INFORMATION, sd);
        RegCloseKey(k);
    }
    LocalFree(sd);
    return e == ERROR_SUCCESS;
}

void Cleanup() {
    SetSddl(kRo, L"D:(A;;KA;;;WD)");   // make the read-only demo key deletable again (owner keeps WRITE_DAC)
    SetSddl(L"Software\\OpenXR-EyeCapture-Test\\RunApproved", L"D:(A;;KA;;;WD)");   // the read-only approval key test, if interrupted
    RegDeleteTreeW(HKEY_CURRENT_USER, kBase);
}

void MakeFile(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

std::wstring ScratchDir(const wchar_t* name, bool json, bool dll) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"EyeCapture-install-test";
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring d = base + L"\\" + name;
    CreateDirectoryW(d.c_str(), nullptr);
    DeleteFileW((d + L"\\" + install::JsonName()).c_str());
    DeleteFileW((d + L"\\" + install::DllName()).c_str());
    if (json) MakeFile(d + L"\\" + install::JsonName());
    if (dll) MakeFile(d + L"\\" + install::DllName());
    return d;
}

void Roots(const wchar_t* lmSub) {
    install::SetTestRoots(std::wstring(L"HKCU\\") + lmSub, std::wstring(L"HKCU\\") + kCu);
}

int LockDemo() {
    Cleanup();
    const std::wstring stale = L"D:\\Old Folder\\" + std::wstring(install::JsonName());
    if (!SetDword(kRo, stale, 0) || !SetDword(kCu, L"placeholder", 1)) { printf("cannot create the demo keys\n"); return 1; }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kCu, L"placeholder");
    if (!SetSddl(kRo, L"D:P(A;;KR;;;WD)")) { printf("cannot set the read-only DACL\n"); return 1; }
    printf("lockdemo: HKCU\\%s is read-only and holds %s\n", N(kRo).c_str(), N(stale).c_str());
    return 0;
}

const wchar_t kApprovedTest[] = L"Software\\OpenXR-EyeCapture-Test\\RunApproved";
const wchar_t kRunTest[] = L"Software\\OpenXR-EyeCapture-Test\\Run";
// Reads a test StartupApproved value: returns its type (REG_NONE if missing), size and first byte.
DWORD ApprovedReadFor(const wchar_t* name, BYTE& b0, DWORD& size) {
    b0 = 0; size = 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedTest, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return REG_NONE;
    BYTE buf[64] = {};
    DWORD type = REG_NONE, sz = sizeof(buf);
    const LSTATUS e = RegQueryValueExW(k, name, nullptr, &type, buf, &sz);
    RegCloseKey(k);
    if (e != ERROR_SUCCESS) return REG_NONE;
    b0 = buf[0]; size = sz;
    return type;
}
DWORD ApprovedRead(BYTE& b0, DWORD& size) { return ApprovedReadFor(install::AutostartValueName(), b0, size); }
// Test setup only (stands in for Windows / Task Manager writing the record; the code under test never does).
void ApprovedWriteByte0For(const wchar_t* name, BYTE v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedTest, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    BYTE rec[12] = {v};
    RegSetValueExW(k, name, 0, REG_BINARY, rec, sizeof(rec));
    RegCloseKey(k);
}
void ApprovedWriteByte0(BYTE v) { ApprovedWriteByte0For(install::AutostartValueName(), v); }
void ApprovedDelete() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedTest, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, install::AutostartValueName());
    RegCloseKey(k);
}
// Test Run key values (the code under test is redirected to kRunTest with SetAutostartTestKey).
void SetRunStr(const wchar_t* name, const std::wstring& v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunTest, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}
bool GetRunStr(const wchar_t* name, std::wstring* v) {
    wchar_t b[1024] = L"";
    DWORD n = sizeof(b) - sizeof(wchar_t);
    const bool ok = RegGetValueW(HKEY_CURRENT_USER, kRunTest, name, RRF_RT_ANY, nullptr, b, &n) == ERROR_SUCCESS;
    if (v) *v = ok ? b : L"";
    return ok;
}

int RunAll() {
    Cleanup();
    const std::wstring a = ScratchDir(L"A", true, true);
    const std::wstring missing = ScratchDir(L"Missing", false, false);
    const std::wstring jsonOnly = ScratchDir(L"JsonOnly", true, false);
    const std::wstring oursA = install::OurValueName(a);
    printf("scratch folder %s\nour value name %s\n", N(a).c_str(), N(oursA).c_str());
    Roots(kLm);
    std::wstring err;
    long we = 0;

    printf("1. nothing registered\n");
    SetDword(kLm, kUnrelated, 0);   // another vendor's layer: must never be touched
    SetDword(kLm, kLookalike, 0);   // file name only LOOKS like ours
    install::Status s = install::Query(a);
    Check(s.state == install::State::NotRegistered, "Query: NotRegistered");
    Check(s.filesPresent, "Query: filesPresent (json + dll in the scratch folder)");
    Check(!s.hkcuDuplicate, "Query: no HKCU duplicate");
    Check(install::OurValueName(a + L"\\") == oursA, "OurValueName ignores a trailing backslash");

    printf("2. Register\n");
    Check(install::Register(a, err, &we), "Register succeeds");
    if (!err.empty()) printf("    err: %s\n", N(err).c_str());
    std::vector<Val> lm = Values(kLm);
    Check(CountSameName(lm) == 1, "exactly one same-name value in LM");
    bool exact = false;
    for (const Val& v : lm) if (v.name == oursA && v.type == REG_DWORD && v.value == 0) exact = true;
    Check(exact, "the value is <dir>\\json = DWORD 0");
    Check(Has(lm, kUnrelated) && Has(lm, kLookalike) && lm.size() == 3, "unrelated + look-alike values untouched");
    s = install::Query(a);
    Check(s.state == install::State::Registered && s.registeredPath == oursA, "Query: Registered, registeredPath = ours");
    Check(install::Register(a, err, &we) && Values(kLm).size() == 3, "Register again: no change");

    printf("3. stale registration from another folder\n");
    Check(install::Unregister(a, err, &we), "Unregister succeeds");
    const std::wstring stale = L"D:\\Old Folder\\" + std::wstring(install::JsonName());
    SetDword(kLm, stale, 0);
    s = install::Query(a);
    Check(s.state == install::State::StalePath && s.registeredPath == stale, "Query: StalePath, registeredPath = the old folder");
    Check(install::Register(a, err, &we), "Register succeeds");
    lm = Values(kLm);
    Check(CountSameName(lm) == 1 && Has(lm, oursA) && !Has(lm, stale), "stale value removed, ours added");
    Check(Has(lm, kUnrelated) && Has(lm, kLookalike), "unrelated values untouched");
    // ours AND a stale one (forward slashes, other case) at the same time: still StalePath until Register cleans up
    const std::wstring stale2 = L"e:/moved/xr_apilayer_jrdevo_EYE_CAPTURE.json";
    SetDword(kLm, stale2, 0);
    s = install::Query(a);
    Check(s.state == install::State::StalePath && s.registeredPath == stale2, "ours + a stale value (/ and other case): StalePath");
    Check(install::Register(a, err, &we), "Register succeeds");
    lm = Values(kLm);
    Check(CountSameName(lm) == 1 && Has(lm, oursA), "only ours left");

    printf("4. ours disabled / ours in another letter case\n");
    SetDword(kLm, oursA, 1);
    s = install::Query(a);
    Check(s.state == install::State::NotRegistered, "ours = 1 (disabled): NotRegistered");
    Check(install::Register(a, err, &we), "Register succeeds");
    lm = Values(kLm);
    bool zero = false;
    for (const Val& v : lm) if (v.name == oursA && v.value == 0) zero = true;
    Check(zero && CountSameName(lm) == 1, "Register set it back to 0");
    std::wstring lower = oursA;
    CharLowerBuffW(&lower[0], (DWORD)lower.size());
    if (lower != oursA) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kLm, oursA.c_str());
        SetDword(kLm, lower, 0);
        s = install::Query(a);
        Check(s.state == install::State::Registered, "ours in lower case: Registered (paths compare case-insensitively)");
        Check(install::Register(a, err, &we), "Register succeeds");
        lm = Values(kLm);
        Check(CountSameName(lm) == 1 && Has(lm, oursA), "Register rewrote it with the exact folder spelling");
    }

    printf("5. HKCU duplicate\n");
    SetDword(kCu, oursA, 0);
    SetDword(kCu, kUnrelated, 0);
    s = install::Query(a);
    Check(s.hkcuDuplicate, "Query: hkcuDuplicate");
    Check(s.state == install::State::DuplicateHkcu, "Query: DuplicateHkcu (registered in LM, also in CU)");
    Check(install::RemoveHkcuDuplicates() == 1, "RemoveHkcuDuplicates removed 1 value");
    std::vector<Val> cu = Values(kCu);
    Check(CountSameName(cu) == 0 && Has(cu, kUnrelated), "CU: same-name value gone, unrelated value kept");
    s = install::Query(a);
    Check(s.state == install::State::Registered && !s.hkcuDuplicate, "Query: Registered again");
    Check(install::RemoveHkcuDuplicates() == 0, "RemoveHkcuDuplicates: nothing left");
    Check(CountSameName(Values(kLm)) == 1, "RemoveHkcuDuplicates never touches LM");
    SetDword(kCu, stale, 0);
    Check(install::Unregister(a, err, &we), "Unregister succeeds");
    s = install::Query(a);
    Check(s.state == install::State::NotRegistered && s.hkcuDuplicate, "CU only: NotRegistered + hkcuDuplicate");
    install::RemoveHkcuDuplicates();

    printf("6. Unregister\n");
    Check(install::Register(a, err, &we), "Register succeeds");
    SetDword(kLm, stale, 0);   // a stale one too: Unregister removes every same-name value
    Check(install::Unregister(a, err, &we), "Unregister succeeds");
    lm = Values(kLm);
    Check(CountSameName(lm) == 0, "no same-name value left in LM");
    Check(Has(lm, kUnrelated) && Has(lm, kLookalike), "unrelated values untouched");
    Check(install::Query(a).state == install::State::NotRegistered, "Query: NotRegistered");
    Check(install::Unregister(a, err, &we), "Unregister with nothing registered succeeds");

    printf("7. files missing\n");
    s = install::Query(missing);
    Check(!s.filesPresent, "Query: filesPresent false (no json, no dll)");
    Check(!install::Query(jsonOnly).filesPresent, "Query: filesPresent false (json but no dll)");
    we = 0;
    Check(!install::Register(missing, err, &we) && we == ERROR_FILE_NOT_FOUND, "Register refuses (ERROR_FILE_NOT_FOUND)");
    printf("    err: %s\n", N(err).c_str());
    Check(CountSameName(Values(kLm)) == 0, "nothing written");

    printf("8. missing root key\n");
    RegDeleteTreeW(HKEY_CURRENT_USER, kLm);
    s = install::Query(a);
    Check(s.state == install::State::NotRegistered, "Query on a missing key: NotRegistered");
    Check(install::Unregister(a, err, &we), "Unregister on a missing key succeeds");
    Check(install::Register(a, err, &we) && CountSameName(Values(kLm)) == 1, "Register creates the key");

    printf("9. no write access (read-only key = what a non-admin sees on HKLM)\n");
    if (LockDemo() == 0) {
        Roots(kRo);
        s = install::Query(a);
        Check(s.state == install::State::StalePath, "Query works read-only: StalePath");
        we = 0;
        Check(!install::Register(a, err, &we) && we == ERROR_ACCESS_DENIED, "Register: ERROR_ACCESS_DENIED");
        printf("    err: %s\n", N(err).c_str());
        Check(err.find(L"needs administrator rights") != std::wstring::npos, "error text says 'needs administrator rights'");
        we = 0;
        Check(!install::Unregister(a, err, &we) && we == ERROR_ACCESS_DENIED, "Unregister: ERROR_ACCESS_DENIED");
        Check(CountSameName(Values(kRo)) == 1, "read-only key unchanged");
        // already exactly registered: Register / Unregister-with-nothing need no write access
        SetSddl(kRo, L"D:(A;;KA;;;WD)");
        RegDeleteTreeW(HKEY_CURRENT_USER, kRo);
        SetDword(kRo, oursA, 0);
        SetSddl(kRo, L"D:P(A;;KR;;;WD)");
        Check(install::Register(a, err, &we), "Register on a read-only key that already holds exactly ours: succeeds (no write)");
    } else {
        Check(false, "could not create the read-only key");
    }

    printf("10. Start with Windows (Run key redirected to a test subkey): only SetAutostart writes the Run value; StartupApproved is read-only\n");
    install::SetAutostartTestKey(kRunTest);
    {
        std::wstring stored = L"junk";
        const std::wstring dirA = L"C:\\A", dirB = L"C:\\B";
        BYTE b0 = 0; DWORD n = 0;
        Check(!install::AutostartEnabled(&stored) && stored.empty(), "not enabled at first (stored command empty)");
        {
            const install::AutostartInfo i0 = install::ReadAutostart();
            Check(!i0.runPresent && !i0.oldPresent && !i0.disabledInTaskManager, "ReadAutostart: nothing there");
            Check(install::AutostartStalePath(dirA).empty(), "no Run value: no stale path");
        }
        Check(install::SetAutostart(true, dirA, err), "SetAutostart(true) succeeds");
        Check(ApprovedRead(b0, n) == REG_NONE, "SetAutostart(true) wrote NO StartupApproved record");
        Check(install::AutostartEnabled(&stored), "enabled");
        Check(stored == install::AutostartCommand(dirA), "stored command == AutostartCommand(C:\\A)");
        Check(install::AutostartCommand(dirA) == L"\"C:\\A\\EyeCaptureTray.exe\"", "command is quoted, no arguments");
        {
            const install::AutostartInfo i1 = install::ReadAutostart();
            Check(install::AutostartPointsHere(i1, dirA) && install::AutostartPointsHere(i1, L"c:\\a"), "points at C:\\A (case-insensitive)");
            Check(!install::AutostartPointsHere(i1, dirB), "does not point at C:\\B");
            Check(install::AutostartStalePath(dirA).empty(), "stale path for C:\\A: none");
            Check(install::AutostartStalePath(dirB) == L"C:\\A\\EyeCaptureTray.exe", "stale path for C:\\B: C:\\A\\EyeCaptureTray.exe");
            Check(GetRunStr(L"VR EyeCapture", &stored) && stored == install::AutostartCommand(dirA), "the read calls changed nothing");
            // the Settings check box asks AutostartPointsHere(ReadAutostart(), ExeFolder()), ExeFolder() = the exe path minus the file
            // name (no trailing backslash); other spellings of the same folder must agree
            Check(install::AutostartPointsHere(i1, L"C:\\A\\") && install::AutostartPointsHere(i1, L"C:/A") &&
                      install::AutostartPointsHere(i1, L"C:\\A\\.") , "points at C:\\A with a trailing backslash / forward slash / dot spelling");
        }
        {   // a real folder (the scratch dir the registration tests use): written by SetAutostart(true), read back by the check-box rule
            wchar_t tmp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tmp);
            std::wstring real = tmp;
            while (real.size() > 3 && real.back() == L'\\') real.pop_back();   // like ExeFolder(): no trailing backslash
            Check(install::SetAutostart(true, real, err), "SetAutostart(true) for a real folder");
            const install::AutostartInfo ir = install::ReadAutostart();
            Check(ir.runPresent && !ir.oldPresent && !ir.disabledInTaskManager, "ReadAutostart: the VR EyeCapture value only");
            Check(install::AutostartPointsHere(ir, real) && install::AutostartPointsHere(ir, real + L"\\"),
                  "real folder: AutostartPointsHere(ReadAutostart(), same folder) is true (the Settings check box ticks)");
            Check(install::SetAutostart(true, dirA, err) && install::AutostartPointsHere(install::ReadAutostart(), dirA),
                  "SetAutostart(true) back to C:\\A");
        }
        // the user disabled it in Task Manager (odd first byte): reads as off, the record is never rewritten
        ApprovedWriteByte0(3);
        Check(!install::AutostartEnabled(&stored) && stored == install::AutostartCommand(dirA), "approved byte0 = 3: AutostartEnabled false, stored command kept");
        Check(install::ReadAutostart().disabledInTaskManager, "ReadAutostart: disabledInTaskManager");
        Check(ApprovedRead(b0, n) == REG_BINARY && n == 12 && b0 == 3, "the reads left the user's record alone");
        Check(install::SetAutostart(true, dirB, err), "SetAutostart(true) C:\\B while disabled in Task Manager");
        Check(ApprovedRead(b0, n) == REG_BINARY && n == 12 && b0 == 3, "... the record is STILL byte0 == 3 (never written)");
        Check(!install::AutostartEnabled(&stored) && stored == install::AutostartCommand(dirB), "... Run value now C:\\B, still reads as disabled");
        ApprovedWriteByte0(2);
        Check(install::AutostartEnabled(), "record byte0 = 2 (set by 'Windows'): enabled");
        ApprovedDelete();
        Check(install::AutostartEnabled(), "record missing: still enabled");
        Check(install::SetAutostart(true, dirA, err) && ApprovedRead(b0, n) == REG_NONE, "SetAutostart(true) with the record missing: no record is created");
        // off: the Run value goes, a record that exists stays exactly as it is
        ApprovedWriteByte0(2);
        Check(install::SetAutostart(false, dirA, err), "SetAutostart(false) succeeds");
        Check(!install::AutostartEnabled(&stored) && stored.empty(), "not enabled again");
        Check(ApprovedRead(b0, n) == REG_BINARY && n == 12 && b0 == 2, "SetAutostart(false) left the StartupApproved record alone");
        ApprovedDelete();
        Check(install::SetAutostart(false, dirA, err), "SetAutostart(false) again: still success");
        // a read-only approval key no longer matters: it is never written
        ApprovedWriteByte0(2);
        if (SetSddl(kApprovedTest, L"D:P(A;;KR;;;WD)")) {
            Check(install::SetAutostart(true, dirA, err) && install::AutostartEnabled(&stored) && stored == install::AutostartCommand(dirA),
                  "read-only approval key: SetAutostart(true) still works (it does not write the key)");
            SetSddl(kApprovedTest, L"D:(A;;KA;;;WD)");
            Check(install::SetAutostart(false, dirA, err) && !install::AutostartEnabled(&stored) && stored.empty(), "SetAutostart(false) cleans up");
        } else {
            Check(false, "could not make the test approval key read-only");
        }
        ApprovedDelete();
    }

    printf("11. the old Run value \"OpenXR-EyeCapture\": only a user tick replaces it; %%APPDATA%% config folder (test keys / folders)\n");
    {
        Check(std::wstring(install::AutostartValueName()) == L"VR EyeCapture", "the Run value is called \"VR EyeCapture\"");
        std::wstring v;
        const std::wstring dirA = L"C:\\A";
        const std::wstring oldCmd = L"\"D:\\Old Place\\EyeCaptureTray.exe\"";
        SetRunStr(L"OpenXR-EyeCapture", oldCmd);
        ApprovedWriteByte0For(L"OpenXR-EyeCapture", 3);   // the user had disabled it in Task Manager
        install::AutostartInfo i = install::ReadAutostart();
        Check(!i.runPresent && i.oldPresent && i.oldCommand == oldCmd, "ReadAutostart sees the old-name value only");
        Check(i.disabledInTaskManager && !install::AutostartEnabled(&v) && v == oldCmd, "its record (old name) says disabled: off, stored command = the old one");
        Check(install::AutostartStalePath(dirA) == L"D:\\Old Place\\EyeCaptureTray.exe", "stale path = the old program path");
        // reading (what the tray does at start) changes nothing
        Check(GetRunStr(L"OpenXR-EyeCapture", &v) && v == oldCmd && !GetRunStr(L"VR EyeCapture", nullptr), "after the reads: old value still there, no new value");
        BYTE b0 = 0; DWORD n = 0;
        Check(ApprovedReadFor(L"OpenXR-EyeCapture", b0, n) == REG_BINARY && n == 12 && b0 == 3, "... and the old-name record is untouched");
        // the user ticks the check box
        std::wstring err;
        Check(install::SetAutostart(true, dirA, err), "user tick: SetAutostart(true)");
        Check(GetRunStr(L"VR EyeCapture", &v) && v == install::AutostartCommand(dirA), "new value written for this folder");
        Check(!GetRunStr(L"OpenXR-EyeCapture", nullptr), "old-name value deleted by the same user action");
        Check(ApprovedReadFor(L"VR EyeCapture", b0, n) == REG_NONE, "no StartupApproved record written for the new name");
        Check(ApprovedReadFor(L"OpenXR-EyeCapture", b0, n) == REG_BINARY && b0 == 3, "the old-name record is left to Windows");
        // both exist, user unticks: both go
        SetRunStr(L"OpenXR-EyeCapture", L"\"X:\\stale\\EyeCaptureTray.exe\"");
        Check(install::SetAutostart(false, dirA, err) && !GetRunStr(L"VR EyeCapture", nullptr) && !GetRunStr(L"OpenXR-EyeCapture", nullptr),
              "user untick: both names deleted");
        {   // leftover record of the old name removed by the test itself
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedTest, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                RegDeleteValueW(k, L"OpenXR-EyeCapture");
                RegCloseKey(k);
            }
        }

        // %APPDATA%: new folder first, then the old one, then next to the DLL; the tray copies old -> new once
        const std::wstring app = ScratchDir(L"AppData", false, false);
        wchar_t oldApp[MAX_PATH] = L"";
        const DWORD had = GetEnvironmentVariableW(L"APPDATA", oldApp, MAX_PATH);
        SetEnvironmentVariableW(L"APPDATA", app.c_str());
        const std::wstring dllDir = app + L"\\dll", oldDir = app + L"\\OpenXR-EyeCapture", newDir = app + L"\\VR EyeCapture";
        CreateDirectoryW(dllDir.c_str(), nullptr);
        Check(ConfigPath(dllDir) == dllDir + L"\\config.ini", "no %APPDATA% copy: config.ini next to the DLL");
        std::wstring from, to;
        Check(MigrateAppDataConfig(&from, &to) == 0, "nothing to copy without an old file");
        CreateDirectoryW(oldDir.c_str(), nullptr);
        { std::ofstream f(oldDir + L"\\config.ini", std::ios::binary); f << "[capture]\r\nbitrate_mbps = 77\r\n"; }
        Check(ConfigPath(dllDir) == oldDir + L"\\config.ini", "only the old folder: its config.ini is used");
        Check(MigrateAppDataConfig(&from, &to) == 1 && from == oldDir + L"\\config.ini" && to == newDir + L"\\config.ini",
              "old file, no new one: copied (folder created)");
        Check(ReadText(newDir + L"\\config.ini") == "[capture]\r\nbitrate_mbps = 77\r\n", "the copy is byte-identical");
        Check(GetFileAttributesW((oldDir + L"\\config.ini").c_str()) != INVALID_FILE_ATTRIBUTES, "the old file is kept");
        Check(ConfigPath(dllDir) == newDir + L"\\config.ini", "both folders: the new one wins");
        { std::ofstream f(newDir + L"\\config.ini", std::ios::binary); f << "[capture]\r\nbitrate_mbps = 88\r\n"; }
        Check(MigrateAppDataConfig() == 0 && ReadText(newDir + L"\\config.ini").find("88") != std::string::npos,
              "second start: nothing copied, the new file is not overwritten");
        DeleteFileW((oldDir + L"\\config.ini").c_str());
        DeleteFileW((newDir + L"\\config.ini").c_str());
        for (const std::wstring& d : {oldDir, newDir, dllDir, app}) RemoveDirectoryW(d.c_str());
        SetEnvironmentVariableW(L"APPDATA", had ? oldApp : nullptr);
    }
    install::SetAutostartTestKey(L"");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test\\RunApproved");

    printf("12. the 32-bit layer (32-bit games): eye_capture32.dll + its json in the 32-bit view (test root <LM>32)\n");
    std::wstring both;
    {
        Roots(kLm);
        RegDeleteTreeW(HKEY_CURRENT_USER, kLm);
        RegDeleteTreeW(HKEY_CURRENT_USER, kLm32);
        both = ScratchDir(L"Both", true, true);
        MakeFile(both + L"\\" + install::JsonName32());
        MakeFile(both + L"\\" + install::DllName32());
        const std::wstring ours64 = install::OurValueName(both), ours32 = install::OurValueName32(both);
        Check(ours32 == both + L"\\XR_APILAYER_JRDEVO_eye_capture32.json", "OurValueName32 = <dir>\\XR_APILAYER_JRDEVO_eye_capture32.json");
        SetDword(kLm32, kUnrelated, 0);
        s = install::Query(both);
        Check(s.filesPresent && s.files32Present, "Query: both file pairs present");
        Check(s.state == install::State::NotRegistered && s.state32 == install::State::NotRegistered, "Query: nothing registered");
        Check(install::Register(both, err, &we), "Register succeeds");
        std::vector<Val> l64 = Values(kLm), l32 = Values(kLm32);
        Check(CountSameName(l64) == 1 && Has(l64, ours64) && CountSameName32(l64) == 0, "64-bit view: only the 64-bit json");
        bool zero32 = false;
        for (const Val& v : l32) if (v.name == ours32 && v.type == REG_DWORD && v.value == 0) zero32 = true;
        Check(zero32 && CountSameName32(l32) == 1 && CountSameName(l32) == 0, "32-bit view: <dir>\\json32 = DWORD 0, nothing else of ours");
        Check(Has(l32, kUnrelated), "32-bit view: unrelated value untouched");
        s = install::Query(both);
        Check(s.state == install::State::Registered && s.state64 == install::State::Registered && s.state32 == install::State::Registered,
              "Query: Registered (both)");
        // an update from a version without the 32-bit layer: the 64-bit one registered, the 32-bit one not
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kLm32, ours32.c_str());
        s = install::Query(both);
        Check(s.state == install::State::NotRegistered && s.state64 == install::State::Registered && s.state32 == install::State::NotRegistered,
              "only the 32-bit part missing: combined NotRegistered, state64 Registered (the tray asks to register)");
        // a stale 32-bit registration from another folder
        const std::wstring stale32 = L"D:\\Old Folder\\" + std::wstring(install::JsonName32());
        SetDword(kLm32, stale32, 0);
        s = install::Query(both);
        Check(s.state == install::State::StalePath && s.registeredPath == stale32 && s.state32 == install::State::StalePath,
              "stale 32-bit value: StalePath, registeredPath = it");
        Check(install::Register(both, err, &we), "Register succeeds");
        l32 = Values(kLm32);
        Check(CountSameName32(l32) == 1 && Has(l32, ours32) && !Has(l32, stale32), "stale 32-bit value replaced by ours");
        Check(install::Query(both).state == install::State::Registered, "Query: Registered again");
        // a folder WITHOUT the 32-bit files: Register leaves the 32-bit view alone, Query ignores it
        Check(install::Register(a, err, &we), "Register (64-bit files only) succeeds");
        l32 = Values(kLm32);
        Check(Has(l32, ours32) && CountSameName32(l32) == 1, "... the 32-bit view is untouched");
        s = install::Query(a);
        Check(!s.files32Present && s.state == install::State::Registered, "... Query(64-bit-only folder): Registered (32-bit not required)");
        // HKCU copies of the 32-bit json are duplicates too
        Check(install::Register(both, err, &we), "Register (both) again");
        SetDword(kCu, ours32, 0);
        s = install::Query(both);
        Check(s.hkcuDuplicate && s.state == install::State::DuplicateHkcu, "HKCU copy of the 32-bit json: DuplicateHkcu");
        Check(install::RemoveHkcuDuplicates() == 1 && CountSameName32(Values(kCu)) == 0, "RemoveHkcuDuplicates removes it");
        // Unregister: both views
        SetDword(kLm32, stale32, 0);
        Check(install::Unregister(both, err, &we), "Unregister succeeds");
        Check(CountSameName(Values(kLm)) == 0 && CountSameName32(Values(kLm32)) == 0, "no value of ours left in either view");
        Check(Has(Values(kLm32), kUnrelated), "32-bit view: unrelated value untouched");
        s = install::Query(both);
        Check(s.state == install::State::NotRegistered && s.state32 == install::State::NotRegistered, "Query: NotRegistered (both)");
    }

    install::SetTestRoots(L"", L"");
    Cleanup();
    for (const std::wstring& d : {a, missing, jsonOnly, both}) {
        DeleteFileW((d + L"\\" + install::JsonName()).c_str());
        DeleteFileW((d + L"\\" + install::DllName()).c_str());
        DeleteFileW((d + L"\\" + install::JsonName32()).c_str());
        DeleteFileW((d + L"\\" + install::DllName32()).c_str());
        RemoveDirectoryW(d.c_str());
    }
    RemoveDirectoryW(a.substr(0, a.find_last_of(L'\\')).c_str());
    HKEY k = nullptr;
    const bool gone = RegOpenKeyExW(HKEY_CURRENT_USER, kBase, 0, KEY_READ, &k) == ERROR_FILE_NOT_FOUND;
    if (k) RegCloseKey(k);
    Check(gone, "test key deleted at the end");
    printf(g_fail ? "install_test: %d FAILED\n" : "install_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"lockdemo") return LockDemo();
    if (mode == L"cleanup") { Cleanup(); return 0; }
    return RunAll();
}
