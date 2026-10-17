// steamvr_games -- see steamvr_games.h.
#include "steamvr_games.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include "openvr/openvr_hooks.h"   // EYECAP_PROXY_REAL_DLL, EYECAP_PROXY_SIDECAR, ProxySidecarText / ReadProxySidecar

#pragma comment(lib, "version.lib")

namespace steamvr {
namespace {

const wchar_t kDll[] = L"openvr_api.dll";
const wchar_t kOcDll[] = L"openvr_api_oc.dll";
const wchar_t kSteamVrBackup[] = L"openvr_api.dll.steamvr-backup";
const wchar_t kOcKeep[] = L"openvr_api.dll.opencomposite";
const wchar_t kNewTmp[] = L"openvr_api.dll.eyecapture-new";
const wchar_t kOldTmp[] = L"openvr_api.dll.eyecapture-old";

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring Join(const std::wstring& d, const wchar_t* n) { return d + L"\\" + n; }
std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring ErrText(DWORD e) {
    if (e == ERROR_ACCESS_DENIED) return L"access denied (the game folder may need administrator rights, or a file is read-only)";
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return L"the file is in use (close the game and SteamVR first)";
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return L"file not found";
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s.empty() ? L"error " + std::to_wstring(e) : s + L" (error " + std::to_wstring(e) + L")";
}

bool ReadAll(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

template <class T> bool At(const std::string& b, size_t off, T& v) {
    if (off + sizeof(T) > b.size() || off + sizeof(T) < off) return false;
    memcpy(&v, b.data() + off, sizeof(T));
    return true;
}

// PE: machine + export names (forwarders "name -> target"). Works on the raw file (no LoadLibrary of a game's DLL).
bool ParsePe(const std::string& b, Machine& machine, std::vector<std::string>& exports) {
    machine = Machine::NotPe;
    exports.clear();
    IMAGE_DOS_HEADER dos{};
    if (!At(b, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    DWORD sig = 0;
    const size_t nt = (size_t)(DWORD)dos.e_lfanew;
    if (!At(b, nt, sig) || sig != IMAGE_NT_SIGNATURE) return false;
    IMAGE_FILE_HEADER fh{};
    if (!At(b, nt + 4, fh)) return false;
    machine = fh.Machine == IMAGE_FILE_MACHINE_AMD64 ? Machine::X64 : fh.Machine == IMAGE_FILE_MACHINE_I386 ? Machine::X86 : Machine::Other;
    const size_t opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    WORD magic = 0;
    if (!At(b, opt, magic)) return true;
    IMAGE_DATA_DIRECTORY exp{};
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 oh{};
        if (!At(b, opt, oh) || oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return true;
        exp = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 oh{};
        if (!At(b, opt, oh) || oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return true;
        exp = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        return true;
    }
    if (!exp.VirtualAddress || !exp.Size) return true;
    const size_t secs = opt + fh.SizeOfOptionalHeader;
    auto rvaToOff = [&](DWORD rva, size_t* off) {
        for (WORD i = 0; i < fh.NumberOfSections && i < 96; ++i) {
            IMAGE_SECTION_HEADER sh{};
            if (!At(b, secs + i * sizeof(IMAGE_SECTION_HEADER), sh)) return false;
            const DWORD size = sh.Misc.VirtualSize ? sh.Misc.VirtualSize : sh.SizeOfRawData;
            if (rva >= sh.VirtualAddress && rva < sh.VirtualAddress + size) {
                *off = (size_t)sh.PointerToRawData + (rva - sh.VirtualAddress);
                return *off < b.size();
            }
        }
        return false;
    };
    auto cstr = [&](DWORD rva) -> std::string {
        size_t off = 0;
        if (!rvaToOff(rva, &off)) return std::string();
        std::string s;
        for (size_t i = off; i < b.size() && b[i] && s.size() < 512; ++i) s += b[i];
        return s;
    };
    size_t eoff = 0;
    IMAGE_EXPORT_DIRECTORY ed{};
    if (!rvaToOff(exp.VirtualAddress, &eoff) || !At(b, eoff, ed)) return true;
    size_t namesOff = 0, ordOff = 0, funcOff = 0;
    if (!rvaToOff(ed.AddressOfNames, &namesOff) || !rvaToOff(ed.AddressOfNameOrdinals, &ordOff) ||
        !rvaToOff(ed.AddressOfFunctions, &funcOff))
        return true;
    for (DWORD i = 0; i < ed.NumberOfNames && i < 4096; ++i) {
        DWORD nameRva = 0;
        WORD ord = 0;
        DWORD fnRva = 0;
        if (!At(b, namesOff + i * 4, nameRva) || !At(b, ordOff + i * 2, ord) || !At(b, funcOff + (size_t)ord * 4, fnRva)) break;
        std::string name = cstr(nameRva);
        if (fnRva >= exp.VirtualAddress && fnRva < exp.VirtualAddress + exp.Size) name += " -> " + cstr(fnRva);   // forwarder
        exports.push_back(name);
    }
    return true;
}

void VersionInfo(const std::wstring& path, std::wstring& company, std::wstring& version) {
    company.clear();
    version.clear();
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return;
    std::vector<BYTE> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return;
    struct Lang { WORD lang, cp; } *langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) || len < sizeof(Lang)) return;
    wchar_t key[96];
    wchar_t* val = nullptr;
    UINT vl = 0;
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\CompanyName", langs[0].lang, langs[0].cp);
    if (VerQueryValueW(buf.data(), key, (void**)&val, &vl) && val) company = val;
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileVersion", langs[0].lang, langs[0].cp);
    if (VerQueryValueW(buf.data(), key, (void**)&val, &vl) && val) version = val;
}

bool HasExport(const DllInfo& d, const char* name, bool* forwarded = nullptr) {
    const size_t n = strlen(name);
    for (const std::string& e : d.exports) {
        if (e.compare(0, n, name) == 0 && (e.size() == n || e.compare(n, 4, " -> ") == 0)) {
            if (forwarded) *forwarded = e.size() > n;
            return true;
        }
    }
    return false;
}

bool MoveStep(const std::wstring& from, const std::wstring& to, bool replace, ActionResult& r) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) {
        r.error = GetLastError();
        r.message = L"cannot rename " + from + L" to " + to + L": " + ErrText(r.error);
        return false;
    }
    r.steps.push_back(L"renamed " + from + L" -> " + to);
    return true;
}

bool CopyFileTo(const std::wstring& from, const std::wstring& to, ActionResult& r) {
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        r.error = GetLastError();
        r.message = L"cannot copy " + from + L" to " + to + L": " + ErrText(r.error);
        return false;
    }
    r.steps.push_back(L"copied " + from + L" -> " + to);
    return true;
}

bool DeleteFileStep(const std::wstring& p, ActionResult& r) {
    if (!DeleteFileW(p.c_str())) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return true;
        r.error = e;
        r.message = L"cannot delete " + p + L": " + ErrText(e);
        return false;
    }
    r.steps.push_back(L"deleted " + p);
    return true;
}

bool WriteSidecar(const std::wstring& dir, const std::wstring& home, const std::wstring& game, ActionResult& r) {
    const std::wstring p = Join(dir, EYECAP_PROXY_SIDECAR);
    const std::string text = ProxySidecarText(home, game);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.error = GetLastError();
        r.message = L"cannot write " + p + L": " + ErrText(r.error);
        return false;
    }
    DWORD w = 0;
    const BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
    const DWORD e = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        r.error = e;
        r.message = L"cannot write " + p + L": " + ErrText(e);
        return false;
    }
    r.steps.push_back(L"wrote " + p);
    return true;
}

// Export names without the forwarder part.
std::vector<std::string> ExportNames(const DllInfo& d) {
    std::vector<std::string> n;
    for (const std::string& e : d.exports) n.push_back(e.substr(0, e.find(" -> ")));
    return n;
}

LocState StateOf(const Location& L) {
    if (L.dll.kind == DllKind::Missing) {
        if (L.real.kind == DllKind::Valve) return LocState::ProxyMissing;   // our proxy deleted, Valve's still renamed
        return LocState::Unknown;
    }
    if (L.dll.machine == Machine::X86) return LocState::Unsupported32;
    if (L.dll.kind == DllKind::Proxy) return L.real.kind == DllKind::Missing ? LocState::OriginalMissing : LocState::Enabled;
    if (L.dll.kind == DllKind::Valve) return (L.real.kind != DllKind::Missing || L.hasSidecar) ? LocState::ProxyMissing : LocState::NotEnabled;
    if (L.dll.kind == DllKind::OpenComposite || L.hasOcDll) return LocState::OpenComposite;   // (a leftover _oc.dll next to Valve's: not active)
    return LocState::Unknown;
}

} // namespace

const char* DllKindName(DllKind k) {
    switch (k) {
    case DllKind::Missing: return "missing";
    case DllKind::Valve: return "Valve OpenVR";
    case DllKind::Proxy: return "VR EyeCapture proxy";
    case DllKind::OpenComposite: return "OpenComposite";
    case DllKind::NotPe: return "not a DLL";
    default: return "unknown";
    }
}

const char* LocStateName(LocState s) {
    switch (s) {
    case LocState::NotEnabled: return "not enabled";
    case LocState::Enabled: return "enabled";
    case LocState::ProxyMissing: return "proxy missing -- game updated? Enable again";
    case LocState::OriginalMissing: return "original missing -- verify the game files in Steam";
    case LocState::OpenComposite: return "OpenComposite installed -- restore SteamVR's openvr_api.dll first";
    case LocState::Unsupported32: return "32-bit -- not supported";
    default: return "unknown openvr_api.dll -- not changed";
    }
}

DllInfo InspectDll(const std::wstring& path) {
    DllInfo d;
    d.path = path;
    std::string b;
    if (!Exists(path) || !ReadAll(path, b)) { d.kind = DllKind::Missing; return d; }
    if (!ParsePe(b, d.machine, d.exports) || d.machine == Machine::NotPe) { d.kind = DllKind::NotPe; return d; }
    VersionInfo(path, d.company, d.fileVersion);
    bool fwd = false;
    if (HasExport(d, EYECAP_PROXY_VERSION_EXPORT)) {
        d.kind = DllKind::Proxy;
        const size_t m = b.find("EYECAP_PROXY_VERSION=");
        if (m != std::string::npos) {
            for (size_t i = m + 21; i < b.size() && b[i] && d.proxyVersion.size() < 32; ++i) d.proxyVersion += b[i];
        }
        return d;
    }
    bool ocForward = false;
    for (const std::string& e : d.exports) {
        const size_t a = e.find(" -> ");
        if (a != std::string::npos && Lower(Widen(e.substr(a + 4))).find(L"_oc.") != std::wstring::npos) ocForward = true;
    }
    if (ocForward || b.find("OpenComposite") != std::string::npos) { d.kind = DllKind::OpenComposite; return d; }
    const bool init2 = HasExport(d, "VR_InitInternal2", &fwd) && !fwd;
    const bool getIf = HasExport(d, "VR_GetGenericInterface", &fwd) && !fwd;
    d.kind = (init2 && getIf) ? DllKind::Valve : DllKind::Unknown;
    return d;
}

Location InspectLocation(const std::wstring& dir, const std::string& ourVersion) {
    Location L;
    L.dir = dir;
    L.dll = InspectDll(Join(dir, kDll));
    L.real = InspectDll(Join(dir, EYECAP_PROXY_REAL_DLL));
    ProxySidecar sc;
    L.hasSidecar = Exists(Join(dir, EYECAP_PROXY_SIDECAR));
    if (L.hasSidecar && ReadProxySidecar(Join(dir, EYECAP_PROXY_SIDECAR), sc)) L.sidecarHome = sc.home;
    L.hasOcDll = Exists(Join(dir, kOcDll));
    L.hasSteamVrBackup = Exists(Join(dir, kSteamVrBackup));
    // leftovers of an interrupted Enable / Disable (a renamed proxy that was still loaded, a copy that was never moved):
    // removed once nothing holds them, else reported
    for (const wchar_t* n : {kOldTmp, kNewTmp}) {
        const std::wstring p = Join(dir, n);
        if (!Exists(p)) continue;
        if (CheckFileAccess(p) == FileAccess::Free && DeleteFileW(p.c_str())) L.staleRemoved.push_back(p);
        else L.staleKept.push_back(p);
    }
    L.state = StateOf(L);
    L.proxyOutdated = L.state == LocState::Enabled && !ourVersion.empty() && VersionLess(L.dll.proxyVersion, ourVersion);
    return L;
}

std::wstring GameRootForExe(const std::wstring& exePath) {
    std::wstring dir = exePath;
    const size_t k = dir.find_last_of(L"\\/");
    dir = k == std::wstring::npos ? L"." : dir.substr(0, k);
    const std::wstring low = Lower(dir);
    const std::wstring marker = L"\\steamapps\\common\\";
    const size_t m = low.find(marker);
    if (m != std::wstring::npos) {
        const size_t start = m + marker.size();
        const size_t end = low.find(L'\\', start);
        return end == std::wstring::npos ? dir : dir.substr(0, end);
    }
    const std::wstring ue = L"\\binaries\\win64";
    if (low.size() > ue.size() && low.compare(low.size() - ue.size(), ue.size(), ue) == 0) {
        std::wstring up = dir.substr(0, dir.size() - ue.size());   // <root>\<Game>
        const size_t s = up.find_last_of(L'\\');
        if (s != std::wstring::npos && s > 2) return up.substr(0, s);
        return up;
    }
    return dir;
}

ScanResult ScanGame(const std::wstring& root, const std::string& ourVersion, int maxDepth, size_t maxEntries, unsigned timeBudgetMs) {
    ScanResult r;
    r.root = root;
    std::vector<std::pair<std::wstring, int>> stack{{root, 0}};
    std::vector<std::wstring> dirs;
    size_t entries = 0;
    const ULONGLONG t0 = GetTickCount64();
    while (!stack.empty()) {
        if (timeBudgetMs && GetTickCount64() - t0 > timeBudgetMs) { r.truncated = true; break; }
        const auto cur = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((cur.first + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool hasDll = false;
        do {
            if (++entries > maxEntries) { r.truncated = true; break; }
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring ln = Lower(n);
            const std::wstring full = cur.first + L"\\" + n;
            if (ln.find(L"easyanticheat") != std::wstring::npos || ln.find(L"battleye") != std::wstring::npos ||
                ln == L"beservice.exe" || ln == L"beservice_x64.exe")
                r.antiCheat.push_back(full);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (cur.second < maxDepth) stack.push_back({full, cur.second + 1});
                else r.truncated = true;
            } else if (ln == L"openvr_api.dll" || ln == L"openvr_api_real.dll") {
                hasDll = true;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (hasDll) dirs.push_back(cur.first);
        if (r.truncated && entries > maxEntries) break;
    }
    for (const std::wstring& d : dirs) r.locations.push_back(InspectLocation(d, ourVersion));
    return r;
}

ActionResult EnableLocation(const Location& L, const std::wstring& proxySource, const std::wstring& home, const std::wstring& game) {
    ActionResult r;
    const std::wstring dll = Join(L.dir, kDll), real = Join(L.dir, EYECAP_PROXY_REAL_DLL), tmp = Join(L.dir, kNewTmp);
    const DllInfo src = InspectDll(proxySource);
    if (src.kind != DllKind::Proxy || src.machine != Machine::X64) {
        r.message = L"the VR EyeCapture proxy " + proxySource + L" is missing or not a 64-bit proxy (reinstall VR EyeCapture)";
        return r;
    }
    switch (L.state) {
    case LocState::Unsupported32: r.message = L"32-bit openvr_api.dll -- not supported, left unchanged"; return r;
    case LocState::OpenComposite: r.message = L"OpenComposite is installed here -- restore SteamVR's openvr_api.dll first"; return r;
    case LocState::OriginalMissing: r.message = L"openvr_api_real.dll (the original) is missing -- verify the game files in Steam, then Enable"; return r;
    case LocState::Unknown: r.message = L"openvr_api.dll here is not Valve's -- left unchanged"; return r;
    default: break;
    }
    if (const DllInfo* g = GameDllOf(L)) {   // the proxy must offer every export the game's own DLL has
        const std::vector<std::string> miss = MissingExports(*g, src);
        if (!miss.empty()) {
            r.message = MissingExportsText(miss);
            return r;
        }
    }
    if (RefuseInUse(L, r)) return r;
    if (L.state == LocState::Enabled) {   // refresh the proxy (update) and the sidecar
        if (!CopyFileTo(proxySource, tmp, r)) return r;
        if (!MoveStep(tmp, dll, true, r)) { DeleteFileW(tmp.c_str()); return r; }
        if (!WriteSidecar(L.dir, home, game, r)) return r;
        r.ok = true;
        r.message = L"already enabled -- proxy and settings refreshed";
        return r;
    }
    if (L.state == LocState::ProxyMissing && L.dll.kind == DllKind::Missing) {   // only openvr_api_real.dll left: put the proxy back
        if (!CopyFileTo(proxySource, dll, r)) return r;
        if (!WriteSidecar(L.dir, home, game, r)) { DeleteFileW(dll.c_str()); return r; }
        r.ok = true;
        r.message = L"enabled (the proxy was missing)";
        return r;
    }
    // NotEnabled, or ProxyMissing with Valve's (updated) DLL back as openvr_api.dll: it becomes the real one
    if (!CopyFileTo(proxySource, tmp, r)) return r;
    if (!MoveStep(dll, real, true, r)) { DeleteFileW(tmp.c_str()); return r; }
    if (!MoveStep(tmp, dll, false, r)) {
        ActionResult rb;
        MoveStep(real, dll, false, rb);   // roll back
        DeleteFileW(tmp.c_str());
        return r;
    }
    if (!WriteSidecar(L.dir, home, game, r)) {
        ActionResult rb;
        DeleteFileW(dll.c_str());
        MoveStep(real, dll, false, rb);
        return r;
    }
    r.ok = true;
    r.message = L.state == LocState::ProxyMissing ? L"enabled again (the game's updated openvr_api.dll is now openvr_api_real.dll)"
                                                  : L"enabled";
    return r;
}

ActionResult DisableLocation(const Location& L) {
    ActionResult r;
    const std::wstring dll = Join(L.dir, kDll), real = Join(L.dir, EYECAP_PROXY_REAL_DLL), side = Join(L.dir, EYECAP_PROXY_SIDECAR);
    if ((L.state == LocState::Enabled || L.state == LocState::ProxyMissing || L.state == LocState::OriginalMissing) && RefuseInUse(L, r))
        return r;   // a loaded DLL can be renamed but not deleted: refuse before the first step, never half-way
    // cleanup after the work is done: a failure there is reported in the text, never as an error of a successful result
    auto tidy = [&](const std::wstring& p, const wchar_t* what) {
        ActionResult t;
        if (!DeleteFileStep(p, t)) { r.message += L" (" + std::wstring(what) + L" " + p + L" could not be deleted: " + ErrText(t.error) + L")"; return; }
        for (const std::wstring& s : t.steps) r.steps.push_back(s);
    };
    switch (L.state) {
    case LocState::Enabled: {
        const std::wstring old = Join(L.dir, kOldTmp);
        if (!MoveStep(dll, old, true, r)) return r;
        if (!MoveStep(real, dll, false, r)) {
            ActionResult rb;
            MoveStep(old, dll, false, rb);
            return r;
        }
        r.ok = true;
        r.message = L"disabled -- the game's own openvr_api.dll is back";
        tidy(old, L"the old proxy");
        tidy(side, L"the sidecar");
        return r;
    }
    case LocState::ProxyMissing:
        if (L.dll.kind == DllKind::Missing) {
            if (!MoveStep(real, dll, false, r)) return r;
        } else if (!DeleteFileStep(real, r)) {
            return r;
        }
        r.ok = true;
        r.message = L"disabled (the proxy was already gone)";
        tidy(side, L"the sidecar");
        return r;
    case LocState::OriginalMissing: {
        const DllInfo b = InspectDll(Join(L.dir, kSteamVrBackup));
        if (b.kind != DllKind::Valve || b.machine != Machine::X64) {
            r.message = L"the original openvr_api.dll (openvr_api_real.dll) is missing -- verify the game files in Steam; that "
                        L"puts Valve's openvr_api.dll back (the proxy is then gone too)";
            return r;
        }
        const std::wstring old = Join(L.dir, kOldTmp);
        if (!MoveStep(dll, old, true, r)) return r;
        if (!CopyFileTo(Join(L.dir, kSteamVrBackup), dll, r)) {
            ActionResult rb;
            MoveStep(old, dll, false, rb);
            return r;
        }
        r.ok = true;
        r.message = L"disabled -- openvr_api.dll restored from openvr_api.dll.steamvr-backup";
        tidy(old, L"the old proxy");
        tidy(side, L"the sidecar");
        return r;
    }
    case LocState::NotEnabled:
        r.ok = true;
        r.message = L"not enabled -- nothing to do";
        if (L.hasSidecar) tidy(side, L"the sidecar");
        return r;
    default:
        r.message = L"not enabled here (" + Widen(LocStateName(L.state)) + L") -- nothing changed";
        return r;
    }
}

ActionResult RestoreSteamVrDll(const Location& L) {
    ActionResult r;
    const std::wstring dll = Join(L.dir, kDll), backup = Join(L.dir, kSteamVrBackup), keep = Join(L.dir, kOcKeep);
    const DllInfo b = InspectDll(backup);
    if (b.kind != DllKind::Valve || b.machine != Machine::X64) {
        r.message = L"no SteamVR copy of openvr_api.dll here (openvr_api.dll.steamvr-backup) -- switch the game back to SteamVR "
                    L"in OpenComposite's switcher, or verify the game files in Steam";
        return r;
    }
    if (L.dll.kind != DllKind::Missing) {
        if (L.dll.kind == DllKind::Valve) { r.ok = true; r.message = L"openvr_api.dll already is SteamVR's"; return r; }
        if (!MoveStep(dll, keep, true, r)) return r;
    }
    if (!CopyFileTo(backup, dll, r)) {
        ActionResult rb;
        if (L.dll.kind != DllKind::Missing) MoveStep(keep, dll, false, rb);
        return r;
    }
    r.ok = true;
    r.message = L"SteamVR's openvr_api.dll restored (OpenComposite's kept as openvr_api.dll.opencomposite)";
    return r;
}

// ------------------------------------------------------------------------------------------------ steamvr_games.ini

bool LoadGameList(const std::wstring& iniPath, std::vector<GameEntry>& out) {
    out.clear();
    std::string bytes;
    if (!ReadAll(iniPath, bytes)) return false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    std::wstringstream ws(Widen(bytes));
    std::wstring line;
    GameEntry* cur = nullptr;
    while (std::getline(ws, line)) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        size_t a = 0;
        while (a < line.size() && (line[a] == L' ' || line[a] == L'\t')) ++a;
        line = line.substr(a);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (Lower(line) == L"[game]") { out.emplace_back(); cur = &out.back(); continue; }
        if (line[0] == L'[') { cur = nullptr; continue; }   // [found] (scan cache) and anything else: not a game entry
        if (!cur) continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == L' ' || k.back() == L'\t')) k.pop_back();
        size_t vb = 0;
        while (vb < v.size() && (v[vb] == L' ' || v[vb] == L'\t')) ++vb;
        v = v.substr(vb);
        k = Lower(k);
        if (k == L"name") cur->name = v;
        else if (k == L"exe") cur->exe = v;
        else if (k == L"root") cur->root = v;
        else if (k == L"dll") cur->dlls.push_back(v);
        else if (k == L"appid") cur->appid = v;
    }
    for (size_t i = out.size(); i-- > 0;) if (out[i].root.empty()) out.erase(out.begin() + (ptrdiff_t)i);
    return true;
}

bool LoadFoundGames(const std::wstring& iniPath, std::vector<FoundGame>& out) {
    out.clear();
    std::string bytes;
    if (!ReadAll(iniPath, bytes)) return false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    std::wstringstream ws(Widen(bytes));
    std::wstring line;
    FoundGame* cur = nullptr;
    while (std::getline(ws, line)) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        size_t a = 0;
        while (a < line.size() && (line[a] == L' ' || line[a] == L'\t')) ++a;
        line = line.substr(a);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (Lower(line) == L"[found]") { out.emplace_back(); cur = &out.back(); continue; }
        if (line[0] == L'[') { cur = nullptr; continue; }
        if (!cur) continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == L' ' || k.back() == L'\t')) k.pop_back();
        size_t vb = 0;
        while (vb < v.size() && (v[vb] == L' ' || v[vb] == L'\t')) ++vb;
        v = v.substr(vb);
        k = Lower(k);
        if (k == L"name") cur->name = v;
        else if (k == L"appid") cur->appid = v;
        else if (k == L"root") cur->root = v;
        else if (k == L"dll") cur->dlls.push_back(v);
        else if (k == L"dll32") cur->dlls32.push_back(v);
        else if (k == L"anticheat") cur->antiCheat = v == L"1";
        else if (k == L"truncated") cur->truncated = v == L"1";
    }
    for (size_t i = out.size(); i-- > 0;) if (out[i].root.empty()) out.erase(out.begin() + (ptrdiff_t)i);
    return true;
}

bool SaveGameList(const std::wstring& iniPath, const std::vector<GameEntry>& games, const std::vector<FoundGame>* found) {
    std::vector<FoundGame> keep;
    if (!found) { LoadFoundGames(iniPath, keep); found = &keep; }
    std::string s = "; VR EyeCapture -- SteamVR games where the proxy was enabled ([game]; bookkeeping only: the files in the\r\n"
                    "; game folders are the truth) and the last Steam library scan ([found]). Written by the tray.\r\n";
    for (const GameEntry& g : games) {
        s += "\r\n[game]\r\nname = " + Narrow(g.name) + "\r\nexe = " + Narrow(g.exe) + "\r\nroot = " + Narrow(g.root) + "\r\n";
        if (!g.appid.empty()) s += "appid = " + Narrow(g.appid) + "\r\n";
        for (const std::wstring& d : g.dlls) s += "dll = " + Narrow(d) + "\r\n";
    }
    for (const FoundGame& f : *found) {
        s += "\r\n[found]\r\nname = " + Narrow(f.name) + "\r\nappid = " + Narrow(f.appid) + "\r\nroot = " + Narrow(f.root) + "\r\n";
        for (const std::wstring& d : f.dlls) s += "dll = " + Narrow(d) + "\r\n";
        for (const std::wstring& d : f.dlls32) s += "dll32 = " + Narrow(d) + "\r\n";
        if (f.antiCheat) s += "anticheat = 1\r\n";
        if (f.truncated) s += "truncated = 1\r\n";
    }
    const std::wstring tmp = iniPath + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), iniPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void UpsertGame(std::vector<GameEntry>& games, const GameEntry& g) {
    for (GameEntry& e : games)
        if (_wcsicmp(e.root.c_str(), g.root.c_str()) == 0) { e = g; return; }
    games.push_back(g);
}

bool RemoveGame(std::vector<GameEntry>& games, const std::wstring& root) {
    for (size_t i = 0; i < games.size(); ++i)
        if (_wcsicmp(games[i].root.c_str(), root.c_str()) == 0) { games.erase(games.begin() + (ptrdiff_t)i); return true; }
    return false;
}

std::wstring GameNameForExe(const std::wstring& exePath) {
    std::wstring s = exePath;
    const size_t k = s.find_last_of(L"\\/");
    if (k != std::wstring::npos) s = s.substr(k + 1);
    const size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos) s = s.substr(0, dot);
    for (const wchar_t* suf : {L"-Win64-Shipping", L"-Win64-Test", L"-Win64-Development"}) {
        const size_t n = wcslen(suf);
        if (s.size() > n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0) s = s.substr(0, s.size() - n);
    }
    return s.empty() ? L"game" : s;
}

// ------------------------------------------------------------------------------------------------ versions

bool VersionLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    for (int part = 0; part < 8; ++part) {
        unsigned long long x = 0, y = 0;
        while (i < a.size() && a[i] >= '0' && a[i] <= '9') x = x * 10 + (unsigned)(a[i++] - '0');
        while (j < b.size() && b[j] >= '0' && b[j] <= '9') y = y * 10 + (unsigned)(b[j++] - '0');
        if (x != y) return x < y;
        while (i < a.size() && a[i] != '.') ++i;   // a non-digit ends the part
        while (j < b.size() && b[j] != '.') ++j;
        if (i >= a.size() && j >= b.size()) return false;
        if (i < a.size()) ++i;
        if (j < b.size()) ++j;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ Steam library

const KvNode* KvNode::Get(const wchar_t* k) const {
    for (const KvNode& c : children)
        if (_wcsicmp(c.key.c_str(), k) == 0) return &c;
    return nullptr;
}

namespace {
struct KvLexer {
    const std::string& s;
    size_t p = 0;
    // 0 = end, '{', '}', 's' = a string (quoted or bare) in `tok`
    int Next(std::string& tok) {
        for (;;) {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
            if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '/') { while (p < s.size() && s[p] != '\n') ++p; continue; }
            break;
        }
        if (p >= s.size()) return 0;
        const char c = s[p];
        if (c == '{' || c == '}') { ++p; return c; }
        tok.clear();
        if (c == '"') {
            ++p;
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size()) {
                    const char e = s[p + 1];
                    tok += e == 'n' ? '\n' : e == 't' ? '\t' : e;   // \\ \" and the rest: the character itself
                    p += 2;
                } else {
                    tok += s[p++];
                }
            }
            if (p < s.size()) ++p;   // closing quote
            return 's';
        }
        while (p < s.size() && s[p] != ' ' && s[p] != '\t' && s[p] != '\r' && s[p] != '\n' && s[p] != '{' && s[p] != '}' && s[p] != '"')
            tok += s[p++];
        return 's';
    }
};
bool ParseKvBlock(KvLexer& lx, KvNode& block, int depth) {
    std::string tok;
    for (;;) {
        const int t = lx.Next(tok);
        if (t == 0) return depth == 0;
        if (t == '}') return depth > 0;
        if (t == '{') return false;   // a block without a key
        KvNode n;
        n.key = Widen(tok);
        std::string v;
        const int t2 = lx.Next(v);
        if (t2 == 's') {
            n.value = Widen(v);
        } else if (t2 == '{') {
            n.isBlock = true;
            if (depth > 32 || !ParseKvBlock(lx, n, depth + 1)) return false;
        } else {
            return false;
        }
        block.children.push_back(std::move(n));
    }
}
std::wstring NormDir(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (p.size() > 3 && (p.back() == L'\\')) p.pop_back();
    return p;
}
} // namespace

bool ParseKeyValues(const std::string& utf8, KvNode& root) {
    root = KvNode();
    root.isBlock = true;
    std::string s = utf8;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    KvLexer lx{s};
    return ParseKvBlock(lx, root, 0);
}

std::vector<std::wstring> ParseLibraryFolders(const std::string& vdfUtf8) {
    std::vector<std::wstring> out;
    KvNode root;
    if (!ParseKeyValues(vdfUtf8, root)) return out;
    const KvNode* lf = root.Get(L"libraryfolders");
    if (!lf || !lf->isBlock) return out;
    auto add = [&](const std::wstring& p) {
        const std::wstring d = NormDir(p);
        if (d.empty()) return;
        for (const std::wstring& x : out) if (_wcsicmp(x.c_str(), d.c_str()) == 0) return;
        out.push_back(d);
    };
    for (const KvNode& c : lf->children) {
        const bool numeric = !c.key.empty() && std::all_of(c.key.begin(), c.key.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
        if (!numeric) continue;   // "TimeNextStatsReport", "ContentStatsID", ...
        if (c.isBlock) {
            if (const KvNode* p = c.Get(L"path")) if (!p->isBlock) add(p->value);
        } else {
            add(c.value);   // the old format: "1" "D:\\SteamLibrary"
        }
    }
    return out;
}

bool ParseAppManifest(const std::string& acfUtf8, AppManifest& out) {
    out = AppManifest();
    KvNode root;
    if (!ParseKeyValues(acfUtf8, root)) return false;
    const KvNode* st = root.Get(L"AppState");
    if (!st || !st->isBlock) return false;
    auto val = [&](const wchar_t* k) { const KvNode* n = st->Get(k); return n && !n->isBlock ? n->value : std::wstring(); };
    out.appid = val(L"appid");
    out.name = val(L"name");
    out.installdir = val(L"installdir");
    if (out.name.empty()) out.name = out.installdir;
    return !out.appid.empty() && !out.installdir.empty() && out.installdir.find(L"..") == std::wstring::npos &&
           out.installdir.find_first_of(L"\\/:") == std::wstring::npos;
}

std::wstring SteamInstallDir(bool testOverride) {
    wchar_t v[MAX_PATH * 2] = L"";
    if (testOverride) return GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_STEAM_DIR", v, MAX_PATH * 2) > 0 ? NormDir(v) : std::wstring();
    DWORD bytes = sizeof(v) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, v, &bytes) == ERROR_SUCCESS && v[0])
        return NormDir(v);
    bytes = sizeof(v) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, v, &bytes) ==
            ERROR_SUCCESS && v[0])
        return NormDir(v);
    return std::wstring();
}

std::vector<std::wstring> SteamLibraries(const std::wstring& steamDir) {
    std::vector<std::wstring> libs;
    if (steamDir.empty()) return libs;
    libs.push_back(NormDir(steamDir));
    std::string vdf;
    if (!ReadAll(steamDir + L"\\steamapps\\libraryfolders.vdf", vdf)) ReadAll(steamDir + L"\\config\\libraryfolders.vdf", vdf);
    for (const std::wstring& l : ParseLibraryFolders(vdf)) {
        bool have = false;
        for (const std::wstring& x : libs) have = have || _wcsicmp(x.c_str(), l.c_str()) == 0;
        if (!have) libs.push_back(l);
    }
    return libs;
}

namespace {
// A Unity (<Game>_Data) or Unreal (Engine) folder within two levels: the engines that ship openvr_api.dll deep inside.
bool HasEngineMarker(const std::wstring& dir, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    std::vector<std::wstring> sub;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring n = Lower(fd.cFileName);
        if (n == L"." || n == L"..") continue;
        if (n == L"engine" || (n.size() > 5 && n.compare(n.size() - 5, 5, L"_data") == 0)) { found = true; break; }
        sub.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (size_t i = 0; !found && depth > 1 && i < sub.size() && i < 64; ++i) found = HasEngineMarker(sub[i], depth - 1);
    return found;
}
} // namespace

std::vector<FoundGame> DiscoverSteamGames(const std::vector<std::wstring>& libraries, const std::string& ourVersion,
                                          const volatile bool* cancel, bool* budgetHit, unsigned totalBudgetMs) {
    std::vector<FoundGame> out;
    if (budgetHit) *budgetHit = false;
    LARGE_INTEGER qf{}, q0{};
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    auto elapsedMs = [&] { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return (unsigned long long)((q.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart); };
    for (const std::wstring& lib : libraries) {
        const std::wstring apps = lib + L"\\steamapps";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((apps + L"\\appmanifest_*.acf").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> files;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(apps + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (const std::wstring& f : files) {
            if (cancel && *cancel) return out;
            if (totalBudgetMs && elapsedMs() >= totalBudgetMs) {   // the whole scan has a budget too (QPC: ms-exact)
                if (budgetHit) *budgetHit = true;
                break;
            }
            std::string acf;
            AppManifest m;
            if (!ReadAll(f, acf) || !ParseAppManifest(acf, m)) continue;
            const std::wstring root = apps + L"\\common\\" + m.installdir;
            if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            bool dup = false;
            for (const FoundGame& g : out) dup = dup || _wcsicmp(g.root.c_str(), root.c_str()) == 0;
            if (dup) continue;
            // cheap first: three levels (most games keep openvr_api.dll next to the exe or in <Game>_Data\Plugins\x86_64);
            // a deep scan only for an incomplete shallow hit or a Unity / Unreal layout
            ScanResult sr = ScanGame(root, ourVersion, 3, 20000, 1000);
            const bool deep = !sr.locations.empty() ? sr.truncated : HasEngineMarker(root, 2);
            if (deep) sr = ScanGame(root, ourVersion, 10, 60000, 4000);
            if (sr.locations.empty()) continue;
            FoundGame g;
            g.appid = m.appid;
            g.name = m.name;
            g.root = root;
            g.antiCheat = !sr.antiCheat.empty();
            g.truncated = sr.truncated;
            for (const Location& L : sr.locations) {
                const bool x86 = L.dll.machine == Machine::X86 || (L.dll.kind == DllKind::Missing && L.real.machine == Machine::X86);
                (x86 ? g.dlls32 : g.dlls).push_back(L.dir);
            }
            out.push_back(std::move(g));
        }
    }
    std::sort(out.begin(), out.end(), [](const FoundGame& a, const FoundGame& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

// ------------------------------------------------------------------------------------------------ automatic repair

FileAccess CheckFileAccess(const std::wstring& path) {
    if (!Exists(path)) return FileAccess::Missing;
    // write + delete access: a loaded DLL (a mapped image) refuses both, although it can still be renamed
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return FileAccess::Free; }
    const DWORD e = GetLastError();
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_USER_MAPPED_FILE) return FileAccess::InUse;
    if (e == ERROR_ACCESS_DENIED) return FileAccess::Denied;   // a protected folder (needs admin) or a read-only file
    return FileAccess::Free;   // anything else: let the real step report it
}

bool FileInUse(const std::wstring& path) { return CheckFileAccess(path) == FileAccess::InUse; }

const DllInfo* GameDllOf(const Location& L) {
    switch (L.state) {
    case LocState::NotEnabled: return &L.dll;
    case LocState::Enabled: return &L.real;
    case LocState::ProxyMissing: return L.dll.kind == DllKind::Valve ? &L.dll : &L.real;
    default: return nullptr;
    }
}

std::vector<std::string> MissingExports(const DllInfo& game, const DllInfo& proxy) {
    const std::vector<std::string> have = ExportNames(proxy);
    std::vector<std::string> miss;
    for (const std::string& n : ExportNames(game)) {
        bool found = false;
        for (const std::string& h : have) found = found || h == n;
        if (!found) miss.push_back(n);
    }
    return miss;
}

std::wstring MissingExportsText(const std::vector<std::string>& miss) {
    std::wstring list;
    for (size_t i = 0; i < miss.size() && i < 4; ++i) list += (i ? L", " : L"") + Widen(miss[i]);
    if (miss.size() > 4) list += L" and " + std::to_wstring(miss.size() - 4) + L" more";
    return L"the game's openvr_api.dll exports " + list + L", which VR EyeCapture's proxy does not; not enabled";
}

bool RefuseInUse(const Location& L, ActionResult& r) {
    for (const wchar_t* n : {kDll, EYECAP_PROXY_REAL_DLL}) {
        const FileAccess a = CheckFileAccess(Join(L.dir, n));
        if (a == FileAccess::InUse) {
            r.error = ERROR_SHARING_VIOLATION;
            r.message = std::wstring(n) + L" is in use -- close the game (and SteamVR), then try again; nothing was changed";
            return true;
        }
        if (a == FileAccess::Denied) {
            r.error = ERROR_ACCESS_DENIED;
            r.message = L"access denied to " + Join(L.dir, n) + L" -- this game folder needs administrator rights (or the file is "
                        L"read-only); nothing was changed";
            return true;
        }
    }
    return false;
}

std::vector<RepairItem> RepairEnabledGames(const std::vector<GameEntry>& games, const std::wstring& proxySource,
                                           const std::wstring& home, const std::string& ourVersion) {
    std::vector<RepairItem> out;
    for (const GameEntry& g : games) {
        for (const std::wstring& d : g.dlls) {
            if (GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            const Location L = InspectLocation(d, ourVersion);
            const bool repair = L.state == LocState::ProxyMissing;
            const bool update = L.state == LocState::Enabled && L.proxyOutdated;
            if (!repair && !update) continue;
            RepairItem it;
            it.game = g.name;
            it.dir = d;
            if (FileInUse(Join(d, kDll)) || FileInUse(Join(d, EYECAP_PROXY_REAL_DLL))) {
                it.what = RepairItem::What::SkippedInUse;
                it.message = L"in use (the game is running) -- tried again next time";
                out.push_back(it);
                continue;
            }
            const ActionResult r = EnableLocation(L, proxySource, home, g.name);
            it.what = !r.ok ? RepairItem::What::Failed : repair ? RepairItem::What::Repaired : RepairItem::What::Updated;
            it.message = r.message;
            out.push_back(it);
        }
    }
    return out;
}

} // namespace steamvr
