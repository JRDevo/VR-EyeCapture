// shared_status -- the tiny IPC between the layer (inside the game) and EyeCaptureTray.exe.
//
// One named shared-memory block (Local\OpenXR-EyeCapture-Status) and one named auto-reset event
// (Local\OpenXR-EyeCapture-Command). The layer process that owns a capture-capable OpenXR session ("owner") publishes
// its status (seqlock-protected) about 4x per second; the tray writes a command + target pid, bumps cmdSeq and sets the
// event. The layer is dormant (creates neither) unless the tray holds EYECAP_TRAY_ALIVE_NAME (or require_tray=0);
// the tray shows "no game" when the owner is gone.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define EYECAP_SHM_NAME   L"Local\\OpenXR-EyeCapture-Status"
#define EYECAP_EVENT_NAME L"Local\\OpenXR-EyeCapture-Command"
// Held open by the GUI tray app for as long as it runs (CLI modes do not hold it). The layer only arms itself for a
// session when this exists (config require_tray, default 1); otherwise it stays dormant = pure pass-through.
#define EYECAP_TRAY_ALIVE_NAME L"Local\\OpenXR-EyeCapture-TrayAlive"
// Per-process recorder markers: named mutexes (never owned, never signalled) kept open for the life of the process. The
// OpenXR layer creates EYECAP_LAYER_MARKER_PREFIX<pid> when it initialises in a process; the SteamVR proxy creates
// EYECAP_PROXY_MARKER_PREFIX<pid> when it claims a process at VR init (one proxy per process). EyeCaptureMirror (SteamVR
// quick mode) checks them for SteamVR's scene application (EyeCapMarkerExists) instead of reading another process.
#define EYECAP_LAYER_MARKER_PREFIX L"Local\\OpenXR-EyeCapture-Layer-"
#define EYECAP_PROXY_MARKER_PREFIX L"Local\\OpenXR-EyeCapture-Proxy-"

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

// The 32-bit layer (eye_capture32.dll, 32-bit games) talks to the 64-bit tray through this block: the layout must be the
// same in both builds (MSVC aligns the 8-byte fields to 8 in x86 structs as well).
static_assert(offsetof(EyeCapShared, framesWritten) == 32 && offsetof(EyeCapShared, game) == 64, "EyeCapShared layout");
static_assert(offsetof(EyeCapShared, cmdSeq) == 2816 && sizeof(EyeCapShared) == 2832, "EyeCapShared layout");

// Opens (or creates) the block. Returns nullptr on failure. Close with UnmapViewOfFile + CloseHandle(*mapping).
// `name` is only changed by tests (a private block so a test tray / status simulator never touch a real game's status).
inline EyeCapShared* EyeCapOpenShared(HANDLE* mapping, const wchar_t* name = EYECAP_SHM_NAME) {
    *mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(EyeCapShared), name);
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

// Layer side: is the GUI tray app running? (Access denied = the object exists but belongs to a more privileged tray.)
inline bool EyeCapTrayRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, EYECAP_TRAY_ALIVE_NAME);
    if (h) { CloseHandle(h); return true; }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

// Does process `pid` hold the marker `prefix`<pid> (EYECAP_LAYER_MARKER_PREFIX / EYECAP_PROXY_MARKER_PREFIX)? Only the object
// namespace is asked (OpenMutexW); nothing is read from the process. Access denied = it exists (a more privileged process).
inline bool EyeCapMarkerExists(const wchar_t* prefix, DWORD pid) {
    wchar_t name[128];
    if (!pid || swprintf_s(name, L"%ls%lu", prefix, (unsigned long)pid) < 0) return false;
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (h) { CloseHandle(h); return true; }
    return GetLastError() == ERROR_ACCESS_DENIED;
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
