// status_sim -- plays the layer's side of the tray IPC without a game or a GPU: publishes ready -> recording -> stopping ->
// "saved <file>" for each MP4 given, so EyeCaptureTray.exe's post-processing can be tested.
//
//   status_sim.exe <shm name> <seconds to stay alive after the last file> <file.mp4> [more.mp4 ...]
//
// Use a private block name (the tray reads it when started with the environment variable EYECAP_TRAY_TEST_SHM=<name>),
// e.g. Local\EyeCapture-Test-Status. Refuses to touch the real block (a real game's status).
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "shared_status.h"

static void Publish(EyeCapShared* s, uint32_t state, const wchar_t* file, const wchar_t* message, bool starting) {
    EyeCapBeginWrite(s);
    s->ownerPid = GetCurrentProcessId();
    s->state = state;
    s->width = 3072; s->height = 3264; s->fps = 90;
    s->recStartTick = starting ? GetTickCount64() : s->recStartTick;
    s->heartbeatTick = GetTickCount64();
    EyeCapCopy(s->game, 64, L"status_sim");
    EyeCapCopy(s->api, 16, L"SIM");
    EyeCapCopy(s->hotkey, 32, L"none");
    EyeCapCopy(s->file, 260, file);
    EyeCapCopy(s->message, 160, message);
    EyeCapEndWrite(s);
}

static void Hold(EyeCapShared* s, int ms) {   // keep the heartbeat fresh
    for (int t = 0; t < ms; t += 100) {
        Sleep(100);
        s->heartbeatTick = GetTickCount64();
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { fwprintf(stderr, L"usage: status_sim <shm name> <hold seconds> <file.mp4> [...]\n"); return 2; }
    if (wcscmp(argv[1], EYECAP_SHM_NAME) == 0) { fwprintf(stderr, L"refusing to use the real status block\n"); return 2; }
    HANDLE map = nullptr;
    EyeCapShared* s = EyeCapOpenShared(&map, argv[1]);
    if (!s) { fwprintf(stderr, L"cannot open the shared block\n"); return 3; }
    Publish(s, ECS_READY, L"", L"", false);
    Hold(s, 1500);
    for (int i = 3; i < argc; ++i) {
        Publish(s, ECS_RECORDING, argv[i], L"", true);
        Hold(s, 1200);
        Publish(s, ECS_STOPPING, argv[i], L"", false);
        Hold(s, 800);
        wchar_t msg[160];
        swprintf_s(msg, L"saved %s", argv[i]);
        Publish(s, ECS_READY, argv[i], msg, false);
        wprintf(L"published saved: %s\n", argv[i]);
        fflush(stdout);
        Hold(s, 1500);
    }
    Hold(s, _wtoi(argv[2]) * 1000);
    EyeCapBeginWrite(s);
    s->ownerPid = 0;
    s->state = ECS_NONE;
    EyeCapEndWrite(s);
    UnmapViewOfFile(s);
    CloseHandle(map);
    return 0;
}
