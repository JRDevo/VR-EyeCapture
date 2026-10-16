// fake_ffmpeg -- a stand-in for ffmpeg.exe in tests/run_hold.ps1 (the tray's post_when hold), so that test needs no
// encoder, no GPU and no real video.
//
//   fake_ffmpeg.exe ... -i <input> ... <output>   "works" for FAKE_FFMPEG_MS milliseconds (default 3000) in 50 ms ticks,
//                                                then copies <input> to <output> (exit 0). Ticks, not wall time: while the
//                                                process is suspended no tick passes, so a suspended job takes longer.
//   fake_ffmpeg.exe -hide_banner -encoders       prints an encoder list without hevc_nvenc (exit 0), or, when
//                                                FAKE_FFMPEG_ENCODERS is set, also one " V....D <name>" line per
//                                                comma-separated name in it (run_yt8k.ps1: "hevc_nvenc,av1_nvenc").
// FAKE_FFMPEG_FAIL_ON=<text>: a run with an argument equal to <text> (e.g. "av1_nvenc") fails at once (stderr line, exit 1),
// like an encoder that is listed but cannot open (run_yt8k.ps1: the av1_nvenc -> hevc_nvenc fallback).
// Every run appends "start <pid>" / "done <pid> <ticks>" to FAKE_FFMPEG_LOG when that is set.
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

static void Note(const char* what, DWORD ticks) {
    wchar_t path[MAX_PATH];
    if (GetEnvironmentVariableW(L"FAKE_FFMPEG_LOG", path, MAX_PATH) == 0) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    fprintf(f, "%s %lu %lu\n", what, GetCurrentProcessId(), ticks);
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i)
        if (wcscmp(argv[i], L"-encoders") == 0) {
            printf("Encoders:\n V..... libx264  fake\n");
            wchar_t list[256];
            const DWORD n = GetEnvironmentVariableW(L"FAKE_FFMPEG_ENCODERS", list, 256);
            if (n > 0 && n < 256) {
                std::wstring rest = list;
                while (!rest.empty()) {
                    const size_t comma = rest.find(L',');
                    const std::wstring name = rest.substr(0, comma);
                    rest = comma == std::wstring::npos ? std::wstring() : rest.substr(comma + 1);
                    if (!name.empty()) printf(" V....D %ls  fake\n", name.c_str());
                }
            }
            return 0;
        }
    wchar_t failOn[64];
    const DWORD fl = GetEnvironmentVariableW(L"FAKE_FFMPEG_FAIL_ON", failOn, 64);
    if (fl > 0 && fl < 64)
        for (int i = 1; i < argc; ++i)
            if (wcscmp(argv[i], failOn) == 0) {
                fprintf(stderr, "fake_ffmpeg: %ls cannot be opened (FAKE_FFMPEG_FAIL_ON)\n", failOn);
                return 1;
            }
    std::wstring in;
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"-i") == 0) in = argv[i + 1];
    if (in.empty() || argc < 3) { fprintf(stderr, "fake_ffmpeg: no -i <input> / output\n"); return 1; }
    const std::wstring out = argv[argc - 1];
    DWORD ms = 3000;
    wchar_t v[32];
    if (GetEnvironmentVariableW(L"FAKE_FFMPEG_MS", v, 32) > 0) ms = (DWORD)wcstoul(v, nullptr, 10);
    Note("start", 0);
    DWORD ticks = 0;
    for (; ticks < ms / 50; ++ticks) Sleep(50);
    if (!CopyFileW(in.c_str(), out.c_str(), FALSE)) {
        fprintf(stderr, "fake_ffmpeg: copy failed (error %lu)\n", GetLastError());
        return 1;
    }
    Note("done", ticks);
    return 0;
}
