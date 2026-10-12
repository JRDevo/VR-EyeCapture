// fake_ffmpeg -- a stand-in for ffmpeg.exe in tests/run_hold.ps1 (the tray's post_when hold), so that test needs no
// encoder, no GPU and no real video.
//
//   fake_ffmpeg.exe ... -i <input> ... <output>   "works" for FAKE_FFMPEG_MS milliseconds (default 3000) in 50 ms ticks,
//                                                then copies <input> to <output> (exit 0). Ticks, not wall time: while the
//                                                process is suspended no tick passes, so a suspended job takes longer.
//   fake_ffmpeg.exe -hide_banner -encoders       prints an encoder list without hevc_nvenc (exit 0).
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
        if (wcscmp(argv[i], L"-encoders") == 0) { printf("Encoders:\n V..... libx264  fake\n"); return 0; }
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
