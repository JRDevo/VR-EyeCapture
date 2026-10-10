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
#include <string>
#include <vector>
#include "install.h"
#include "log.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
std::string N(const std::wstring& w) { return elog::Narrow(w); }

const wchar_t kBase[] = L"Software\\OpenXR-EyeCapture-Test";
const wchar_t kLm[] = L"Software\\OpenXR-EyeCapture-Test\\LM";
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
    RegDeleteTreeW(HKEY_CURRENT_USER, kBase);
}

void MakeFile(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

std::wstring ScratchDir(const wchar_t* name, bool json, bool dll) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"OpenXR-EyeCapture-install-test";
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

    printf("10. Start with Windows (Run key redirected to a test subkey)\n");
    install::SetAutostartTestKey(L"Software\\OpenXR-EyeCapture-Test\\Run");
    {
        std::wstring stored = L"junk";
        const std::wstring dirA = L"C:\\A", dirB = L"C:\\B";
        Check(!install::AutostartEnabled(&stored) && stored.empty(), "not enabled at first (stored command empty)");
        Check(!install::SyncAutostart(dirA), "SyncAutostart when not enabled: no-op");
        Check(!install::AutostartEnabled(), "SyncAutostart did not turn it on");
        Check(install::SetAutostart(true, dirA, err), "SetAutostart(true) succeeds");
        Check(install::AutostartEnabled(&stored), "enabled");
        Check(stored == install::AutostartCommand(dirA), "stored command == AutostartCommand(C:\\A)");
        Check(install::AutostartCommand(dirA) == L"\"C:\\A\\EyeCaptureTray.exe\"", "command is quoted, no arguments");
        Check(!install::SyncAutostart(dirA), "SyncAutostart with the same folder: false (nothing rewritten)");
        Check(install::SyncAutostart(dirB), "SyncAutostart with another folder: true (rewritten)");
        Check(install::AutostartEnabled(&stored) && stored == install::AutostartCommand(dirB) && stored.find(L"C:\\B") != std::wstring::npos,
              "stored command now names C:\\B");
        Check(!install::SyncAutostart(L"c:\\b"), "SyncAutostart: case-insensitive compare");
        Check(install::SetAutostart(false, dirA, err), "SetAutostart(false) succeeds");
        Check(!install::AutostartEnabled(&stored) && stored.empty(), "not enabled again");
        Check(install::SetAutostart(false, dirA, err), "SetAutostart(false) again: still success");
    }
    install::SetAutostartTestKey(L"");

    install::SetTestRoots(L"", L"");
    Cleanup();
    for (const std::wstring& d : {a, missing, jsonOnly}) {
        DeleteFileW((d + L"\\" + install::JsonName()).c_str());
        DeleteFileW((d + L"\\" + install::DllName()).c_str());
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
