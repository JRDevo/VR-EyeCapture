// log -- see log.h.
#include "log.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace elog {
namespace {
std::mutex   s_lock;
HANDLE       s_primary = INVALID_HANDLE_VALUE;
HANDLE       s_mirror  = INVALID_HANDLE_VALUE;
std::wstring s_primaryPath;
std::wstring s_mirrorPath;
bool         s_echo = false;
LogLevel     s_level = LogLevel::Info;

void EnsureParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = path.substr(0, slash);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);   // ok if it exists
}

HANDLE OpenAppend(const std::wstring& path, bool rotate) {
    EnsureParentDir(path);
    if (rotate) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
            (fa.nFileSizeHigh > 0 || fa.nFileSizeLow > 4u * 1024u * 1024u)) {
            const std::wstring old = path + L".old";
            MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);   // fails harmlessly if another process holds it
        }
    }
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

const char* LevelName(LogLevel l) {
    switch (l) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Info:  return "INFO ";
    default:              return "DEBUG";
    }
}
} // namespace

void Init(const std::wstring& primaryPath, bool echoStdout) {
    std::lock_guard<std::mutex> lk(s_lock);
    if (s_primary != INVALID_HANDLE_VALUE) CloseHandle(s_primary);
    s_primaryPath = primaryPath;
    s_primary = OpenAppend(primaryPath, true);
    s_echo = echoStdout;
}

void SetMirror(const std::wstring& mirrorPath) {
    std::lock_guard<std::mutex> lk(s_lock);
    if (mirrorPath == s_mirrorPath) return;
    if (s_mirror != INVALID_HANDLE_VALUE) { CloseHandle(s_mirror); s_mirror = INVALID_HANDLE_VALUE; }
    s_mirrorPath = mirrorPath;
    if (!mirrorPath.empty() && _wcsicmp(mirrorPath.c_str(), s_primaryPath.c_str()) != 0)
        s_mirror = OpenAppend(mirrorPath, true);
}

void SetLevel(LogLevel level) { s_level = level; }
LogLevel Level() { return s_level; }
bool Enabled(LogLevel level) { return (int)level <= (int)s_level; }
std::wstring PrimaryPath() { return s_primaryPath; }

void Write(LogLevel level, const char* fmt, ...) {
    if (!Enabled(level)) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    const int len = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %5lu] %s %s\r\n",
                             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                             GetCurrentProcessId(), LevelName(level), msg);
    if (len <= 0) return;
    const DWORD bytes = (DWORD)((size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1);

    std::lock_guard<std::mutex> lk(s_lock);
    DWORD written = 0;
    if (s_primary != INVALID_HANDLE_VALUE) WriteFile(s_primary, line, bytes, &written, nullptr);
    if (s_mirror != INVALID_HANDLE_VALUE) WriteFile(s_mirror, line, bytes, &written, nullptr);
    if (s_echo) { fwrite(line, 1, bytes, stdout); fflush(stdout); }
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
} // namespace elog
