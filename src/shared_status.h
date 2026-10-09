// shared_status -- the tiny IPC between the layer (inside the game) and EyeCaptureTray.exe.
//
// One named shared-memory block (Local\OpenXR-EyeCapture-Status) and one named auto-reset event
// (Local\OpenXR-EyeCapture-Command). The layer process that owns a capture-capable OpenXR session ("owner") publishes
// its status (seqlock-protected) about 4x per second; the tray writes a command + target pid, bumps cmdSeq and sets the
// event. The layer works the same with no tray running; the tray shows "no game" when the owner is gone.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

#define EYECAP_SHM_NAME   L"Local\\OpenXR-EyeCapture-Status"
#define EYECAP_EVENT_NAME L"Local\\OpenXR-EyeCapture-Command"

enum : uint32_t { EYECAP_MAGIC = 0x50414345u /* 'ECAP' */, EYECAP_VERSION = 1 };

enum EyeCapState : uint32_t {
    ECS_NONE = 0,          // no game with a capture-capable session
    ECS_READY = 1,         // game running, not recording
    ECS_STARTING = 2,
    ECS_RECORDING = 3,
    ECS_STOPPING = 4,      // finalizing the MP4
    ECS_FAILED = 5,        // last start failed (message says why)
};

enum EyeCapCmd : uint32_t { ECC_NONE = 0, ECC_TOGGLE = 1, ECC_START = 2, ECC_STOP = 3 };

struct EyeCapShared {
    uint32_t magic;
    uint32_t version;
    volatile LONG seq;          // seqlock: odd while the owner writes the status fields below
    uint32_t ownerPid;
    uint32_t state;             // EyeCapState
    uint32_t width, height, fps;
    uint64_t framesWritten, framesDropped;
    uint64_t recStartTick;      // GetTickCount64() at record start (system-wide clock), 0 = not recording
    uint64_t heartbeatTick;     // GetTickCount64() of the last publish
    wchar_t  game[64];
    wchar_t  api[16];
    wchar_t  runtime[64];
    wchar_t  hotkey[32];
    wchar_t  file[260];
    wchar_t  outputDir[260];
    wchar_t  logPath[260];
    wchar_t  configPath[260];
    wchar_t  message[160];
    // command block (written by the tray)
    volatile LONG cmdSeq;
    uint32_t cmdTargetPid;
    uint32_t cmd;               // EyeCapCmd
};

// Opens (or creates) the block. Returns nullptr on failure. Close with UnmapViewOfFile + CloseHandle(*mapping).
inline EyeCapShared* EyeCapOpenShared(HANDLE* mapping) {
    *mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(EyeCapShared), EYECAP_SHM_NAME);
    if (!*mapping) return nullptr;
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    auto* p = (EyeCapShared*)MapViewOfFile(*mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EyeCapShared));
    if (!p) { CloseHandle(*mapping); *mapping = nullptr; return nullptr; }
    if (created || p->magic != EYECAP_MAGIC) {
        // zero-filled by the OS on creation; stamp it
        p->magic = EYECAP_MAGIC;
        p->version = EYECAP_VERSION;
    }
    return p;
}

// Consistent copy of the status part (everything up to cmdSeq). Returns false if the writer kept it busy.
inline bool EyeCapReadStatus(const EyeCapShared* p, EyeCapShared& out) {
    for (int tries = 0; tries < 100; ++tries) {
        const LONG s1 = p->seq;
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(&out, (const void*)p, offsetof(EyeCapShared, cmdSeq));
        MemoryBarrier();
        if (p->seq == s1) return true;
    }
    return false;
}

inline void EyeCapBeginWrite(EyeCapShared* p) { InterlockedIncrement(&p->seq); MemoryBarrier(); }
inline void EyeCapEndWrite(EyeCapShared* p) { MemoryBarrier(); InterlockedIncrement(&p->seq); }

inline void EyeCapCopy(wchar_t* dst, size_t cap, const wchar_t* src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}

inline bool EyeCapProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// Tray side: send a command to the current owner. Returns false if no live owner.
inline bool EyeCapSendCommand(EyeCapShared* p, uint32_t cmd) {
    const DWORD owner = p->ownerPid;
    if (!EyeCapProcessAlive(owner)) return false;
    p->cmdTargetPid = owner;
    p->cmd = cmd;
    MemoryBarrier();
    InterlockedIncrement(&p->cmdSeq);
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, EYECAP_EVENT_NAME);
    if (e) { SetEvent(e); CloseHandle(e); }
    return true;
}
