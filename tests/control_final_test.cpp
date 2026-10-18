// control_final_test -- the layer's control thread + ControlPublishFinal on a private status block, without a GPU, a game
// or the OpenXR runtime. Reproduces the shutdown race that lost "saved <file>": the session is destroyed (control thread no
// longer sees a capture-capable session, it releases ownership), the recorder's result is handed over with
// ControlPublishFinal, the control thread is stopped at once -- the tray must still find message + file in the block.
// Also checks that a Stopping -> Idle transition is published immediately (hotkey stop path), not after 250 ms.
// Exit code 0 = pass.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include "control.h"
#include "log.h"
#include "shared_status.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::mutex g_m;
static CaptureSnapshot g_snap;
static void SetSnap(bool capable, RecState st, const char* msg, const wchar_t* file) {
    std::lock_guard<std::mutex> lk(g_m);
    g_snap = CaptureSnapshot();
    g_snap.capable = capable;
    g_snap.rec.state = st;
    g_snap.rec.message = msg;
    g_snap.rec.file = file;
    g_snap.game = L"control_final_test";
    g_snap.outputDir = L"C:\\captures";
}

static ControlHooks Hooks() {
    ControlHooks h;
    h.snapshot = [] { std::lock_guard<std::mutex> lk(g_m); return g_snap; };
    h.requestStart = [] {};
    h.requestStop = [] {};
    return h;
}

static bool Read(EyeCapShared* shm, EyeCapShared& out) { return EyeCapReadStatus(shm, out); }

int wmain() {
    const wchar_t* name = L"Local\\EyeCapture-ControlFinalTest";
    SetEnvironmentVariableW(L"XR_EYE_CAPTURE_TEST_SHM", name);
    HANDLE map = nullptr;
    EyeCapShared* shm = EyeCapOpenShared(&map, name);
    if (!shm) { printf("cannot open the private block\n"); return 2; }
    Config cfg;
    ControlStart(cfg, L"log", L"cfg", Hooks());

    printf("case 1: session destroyed right after the recording (the lost-message race)\n");
    SetSnap(true, RecState::Recording, "", L"C:\\captures\\a_SBS.mp4");
    Sleep(600);
    EyeCapShared st{};
    Check(Read(shm, st) && st.ownerPid == GetCurrentProcessId() && st.state == ECS_RECORDING, "recording published, we own the block");
    SetSnap(false, RecState::Idle, "", L"");                 // session removed from the layer's maps (Hook_DestroySession)
    Sleep(150);                                              // the loop releases ownership while the file finalizes
    Check(Read(shm, st) && st.ownerPid == 0 && st.state == ECS_NONE, "ownership released during the finalize");
    RecorderStatus rs;
    rs.state = RecState::Idle;
    rs.file = L"C:\\captures\\a_SBS.mp4";
    rs.message = "saved C:\\captures\\a_SBS.mp4";
    rs.framesWritten = 1234;
    ControlPublishFinal(rs, L"C:\\captures");
    ControlStop();                                           // StopControlIfIdle right after, within ms
    Check(Read(shm, st) && wcscmp(st.message, L"saved C:\\captures\\a_SBS.mp4") == 0, "message 'saved <file>' survives the shutdown");
    Check(wcscmp(st.file, L"C:\\captures\\a_SBS.mp4") == 0 && st.framesWritten == 1234, "file + counters survive");
    Check(st.ownerPid == 0 && st.state == ECS_NONE, "only ownerPid / state were cleared");

    printf("case 2: still the owner when the final status arrives, then shutdown\n");
    ControlStart(cfg, L"log", L"cfg", Hooks());
    SetSnap(true, RecState::Recording, "", L"C:\\captures\\b_SBS.mp4");
    Sleep(600);
    Check(Read(shm, st) && st.ownerPid == GetCurrentProcessId(), "owner again");
    rs.file = L"C:\\captures\\b_SBS.mp4";
    rs.message = "saved C:\\captures\\b_SBS.mp4";
    SetSnap(true, RecState::Idle, "saved C:\\captures\\b_SBS.mp4", L"C:\\captures\\b_SBS.mp4");   // the recorder reports it too
    ControlPublishFinal(rs, L"C:\\captures");                // game thread, loop may be mid-iteration
    ControlStop();
    Check(Read(shm, st) && wcscmp(st.message, L"saved C:\\captures\\b_SBS.mp4") == 0 && st.ownerPid == 0,
          "message intact, ownership released at shutdown");

    printf("case 3: hotkey stop -- Stopping -> Idle is published at once (not on the 250 ms grid)\n");
    ControlStart(cfg, L"log", L"cfg", Hooks());
    SetSnap(true, RecState::Stopping, "", L"C:\\captures\\c_SBS.mp4");
    Sleep(600);
    Check(Read(shm, st) && st.state == ECS_STOPPING, "stopping published");
    SetSnap(true, RecState::Idle, "saved C:\\captures\\c_SBS.mp4", L"C:\\captures\\c_SBS.mp4");
    Sleep(90);
    Check(Read(shm, st) && wcscmp(st.message, L"saved C:\\captures\\c_SBS.mp4") == 0 && st.state == ECS_READY,
          "saved published within 90 ms");
    ControlStop();

    // the start / stop / error cues: WAV files PlaySound can play from memory
    for (int k = 0; k < 3; ++k) {
        const std::vector<uint8_t> w = BuildCueWav(k);
        const uint32_t riff = w.size() >= 44 ? *(const uint32_t*)&w[4] : 0, data = w.size() >= 44 ? *(const uint32_t*)&w[40] : 0;
        const int ms = (int)(data / 2 * 1000 / 48000);
        const int wantMs = k == 0 ? 160 : k == 1 ? 250 : 450;
        char what[128];
        snprintf(what, sizeof what, "cue %d: RIFF/WAVE PCM 16-bit mono 48 kHz, sizes consistent, %d ms (want %d)", k, ms, wantMs);
        Check(w.size() > 44 && memcmp(&w[0], "RIFF", 4) == 0 && memcmp(&w[8], "WAVEfmt ", 8) == 0 && memcmp(&w[36], "data", 4) == 0 &&
              *(const uint16_t*)&w[20] == 1 && *(const uint16_t*)&w[22] == 1 && *(const uint32_t*)&w[24] == 48000 &&
              *(const uint16_t*)&w[34] == 16 && riff == w.size() - 8 && data == w.size() - 44 && ms >= wantMs - 2 && ms <= wantMs + 2, what);
        Check(*(const int16_t*)&w[44] == 0, "cue starts at silence (fade in)");
    }

    UnmapViewOfFile(shm);
    CloseHandle(map);
    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
