// status_sim -- plays the layer's side of the tray IPC without a game or a GPU: publishes ready -> recording -> stopping ->
// "saved <file>" for each MP4 given, so EyeCaptureTray.exe's post-processing can be tested.
//
//   status_sim.exe <shm name> <seconds to stay alive after the last file> [--vanish] [--linger <s>] [--outdir <dir>] <file.mp4> [more.mp4 ...]
//
//   --vanish   the game quits right after the recording: ~50 ms after publishing "saved <file>" ownerPid / state are cleared
//              (message + file stay), like the layer's shutdown path. Tests that the tray still queues the conversion.
//   --outdir   publish <dir> as the captures folder (outputDir) in the block.
//   --linger <s>  after releasing the block at the end (ownerPid 0, ECS_NONE: the layer's "session destroyed" state),
//              keep the process alive for <s> more seconds -- a game that left VR but is still running (post_when tests).
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
    if (argc < 3) { fwprintf(stderr, L"usage: status_sim <shm name> <hold seconds> [--vanish] [--linger <s>] [--outdir <dir>] [file.mp4 ...]\n"); return 2; }
    if (wcscmp(argv[1], EYECAP_SHM_NAME) == 0) { fwprintf(stderr, L"refusing to use the real status block\n"); return 2; }
    HANDLE map = nullptr;
    EyeCapShared* s = EyeCapOpenShared(&map, argv[1]);
    if (!s) { fwprintf(stderr, L"cannot open the shared block\n"); return 3; }
    bool vanish = false;
    int linger = 0;
    int first = 3;
    for (; first < argc; ++first) {
        if (wcscmp(argv[first], L"--vanish") == 0) vanish = true;
        else if (wcscmp(argv[first], L"--linger") == 0 && first + 1 < argc) linger = _wtoi(argv[++first]);
        else if (wcscmp(argv[first], L"--outdir") == 0 && first + 1 < argc) { EyeCapCopy(s->outputDir, 260, argv[++first]); }
        else break;
    }
    Publish(s, ECS_READY, L"", L"", false);
    Hold(s, 1500);
    for (int i = first; i < argc; ++i) {
        Publish(s, ECS_RECORDING, argv[i], L"", true);
        Hold(s, 1200);
        Publish(s, ECS_STOPPING, argv[i], L"", false);
        Hold(s, 800);
        wchar_t msg[160];
        swprintf_s(msg, L"saved %s", argv[i]);
        Publish(s, ECS_READY, argv[i], msg, false);
        wprintf(L"published saved: %s\n", argv[i]);
        fflush(stdout);
        if (vanish) {
            Sleep(50);
            EyeCapBeginWrite(s);
            s->ownerPid = 0;      // message + file stay, as in the layer's shutdown path
            s->state = ECS_NONE;
            EyeCapEndWrite(s);
            wprintf(L"owner gone 50 ms after saved\n");
            fflush(stdout);
            Sleep(1500);
            continue;
        }
        Hold(s, 1500);
    }
    Hold(s, _wtoi(argv[2]) * 1000);
    EyeCapBeginWrite(s);
    s->ownerPid = 0;
    s->state = ECS_NONE;
    EyeCapEndWrite(s);
    if (linger > 0) {
        wprintf(L"released the block; lingering %d s\n", linger);
        fflush(stdout);
        Sleep((DWORD)linger * 1000);
    }
    UnmapViewOfFile(s);
    CloseHandle(map);
    return 0;
}
