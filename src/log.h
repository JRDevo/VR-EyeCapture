// log -- thread-safe, multi-process-safe append log for the layer and the tests.
//
// One primary file (<dll folder>\logs\eye_capture.log) plus an optional mirror (<output_dir>\eye_capture.log),
// both opened for append with full sharing so several OpenXR processes can write at once. Every line carries
// a timestamp, the process id and the level. Rotation: a file over 4 MB is renamed to .old at Init.
#pragma once
#include <string>

// None = write nothing at all (every real level is >= 0, so Enabled() is false for all of them).
enum class LogLevel { None = -1, Error = 0, Warn = 1, Info = 2, Debug = 3 };

namespace elog {
// Opens the primary log (creates the folder). echoStdout = also print to stdout (tests).
void Init(const std::wstring& primaryPath, bool echoStdout);
// Adds / replaces the mirror file (empty = none). Safe to call at any time.
void SetMirror(const std::wstring& mirrorPath);
void SetLevel(LogLevel level);
LogLevel Level();
bool Enabled(LogLevel level);
void Write(LogLevel level, const char* fmt, ...);
std::wstring PrimaryPath();
// UTF-16 -> UTF-8 for log arguments.
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
}

#define LOGE(...) elog::Write(LogLevel::Error, __VA_ARGS__)
#define LOGW(...) elog::Write(LogLevel::Warn, __VA_ARGS__)
#define LOGI(...) elog::Write(LogLevel::Info, __VA_ARGS__)
#define LOGD(...) do { if (elog::Enabled(LogLevel::Debug)) elog::Write(LogLevel::Debug, __VA_ARGS__); } while (0)
