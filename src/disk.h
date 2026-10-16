// disk -- free space of the drive a recording / conversion writes to, and the low-space thresholds.
//
// Shared by the layer (src/recorder.cpp: a recording does not start below MinFreeToStart() and stops cleanly when the drive
// drops below MinFreeToContinue() while recording), the tray's conversion worker (tray/postprocess.cpp: a conversion does not
// start below MinFreeToStart()) and the Settings window (free space next to the output folder).
// Tests: EYECAP_TEST_MIN_FREE_GB / EYECAP_TEST_KEEP_FREE_GB override the two thresholds (whole GB), so the guards can be
// exercised without filling a disk.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

namespace disk {

constexpr unsigned long long kGB = 1024ull * 1024ull * 1024ull;   // Explorer's "GB"

inline unsigned long long EnvGB(const wchar_t* name, unsigned long long def) {
    wchar_t v[32];
    const DWORD n = GetEnvironmentVariableW(name, v, 32);
    if (n == 0 || n >= 32) return def * kGB;
    return wcstoull(v, nullptr, 10) * kGB;
}
// a recording / a conversion does not start with less free space than this (10 GB)
inline unsigned long long MinFreeToStart() { return EnvGB(L"EYECAP_TEST_MIN_FREE_GB", 10); }
// a running recording is stopped (and finalized) when the free space drops below this (2 GB)
inline unsigned long long MinFreeToContinue() { return EnvGB(L"EYECAP_TEST_KEEP_FREE_GB", 2); }

// "E:" for a drive path, "\\server\share" for a UNC path, "" otherwise.
inline std::wstring DriveName(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return path.substr(0, 2);
    if (path.size() > 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/')) {
        const size_t a = path.find_first_of(L"\\/", 2);
        const size_t b = a == std::wstring::npos ? a : path.find_first_of(L"\\/", a + 1);
        return path.substr(0, b);
    }
    return L"";
}

// Free bytes available to this user on the drive that holds `path` (a file or a folder; a folder that does not exist yet
// is resolved through its nearest existing parent). False when no part of the path exists.
inline bool FreeBytes(std::wstring path, unsigned long long& freeBytes) {
    for (int guard = 0; guard < 64 && !path.empty(); ++guard) {
        ULARGE_INTEGER avail{};
        if (GetDiskFreeSpaceExW(path.c_str(), &avail, nullptr, nullptr)) {
            freeBytes = avail.QuadPart;
            return true;
        }
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();   // "E:\a\" -> "E:\a"
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        path.resize(slash + 1);   // the parent, with its separator ("E:\a\b" -> "E:\a\", "E:\a" -> "E:\")
        // a drive root ("E:\") and a UNC share root ("\\server\share\") keep it: GetDiskFreeSpaceExW wants the trailing
        // separator there ("\\server\share" without it fails, and the walk would go on to "\\server" and give up)
        if (path.size() > 3 && path.size() != DriveName(path).size() + 1) path.pop_back();
    }
    return false;
}

// "812 GB" / "8.2 GB" / "950 MB".
inline std::wstring SizeText(unsigned long long bytes) {
    wchar_t b[32];
    const double gb = (double)bytes / (double)kGB;
    if (gb >= 100.0) swprintf_s(b, L"%.0f GB", gb);
    else if (gb >= 1.0) swprintf_s(b, L"%.1f GB", gb);
    else swprintf_s(b, L"%.0f MB", (double)bytes / (1024.0 * 1024.0));
    return b;
}

// "E: 812 GB free" ("? GB free" style text when unknown: "free space unknown").
inline std::wstring FreeText(const std::wstring& path) {
    unsigned long long f = 0;
    if (!FreeBytes(path, f)) return L"free space unknown";
    const std::wstring d = DriveName(path);
    return (d.empty() ? std::wstring() : d + L" ") + SizeText(f) + L" free";
}

} // namespace disk
