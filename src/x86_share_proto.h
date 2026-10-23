// x86_share_proto -- 32-bit OpenXR games (docs/X86_DESIGN.md): the shared-memory protocol between the 32-bit layer
// eye_capture32.dll inside the game (the PRODUCER: src/recorder_remote.cpp, a Recorder that hands the eye images away) and
// the 64-bit helper EyeCaptureRecorder.exe (the CONSUMER: src/recorder_helper.cpp, which runs the normal Recorder). The same
// header is compiled 32-bit and 64-bit: fixed-size fields only, every 8-byte field at an 8-byte offset, no pointers /
// HANDLEs / bool; the static_asserts at the end pin the layout so both builds agree byte for byte.
//
// Objects (the names keep the old spelling, docs/RENAME_PLAN.md; tests replace <block> with XR_EYE_CAPTURE_TEST_X86_SHM):
//   <block>            = Local\OpenXR-EyeCapture-X86   the block below, CREATED BY THE PRODUCER (the layer) when its first
//                                                      armed session gets a recorder; the tray sees it exist and starts
//                                                      the helper (tray.cpp X86Tick: one OpenFileMappingW every 2 s)
//   <block>-Wake       auto-reset event                producer -> helper: a command or a frame was published
//   <block>-Quit       manual-reset event              tray -> helper: finish (finalize a recording) and exit
//   <block>-Recorder   mutex                           the helper's single instance
//
// Roles. The game keeps everything a 64-bit game does in process: the control thread (hotkey, tray commands, status block,
// cues), the per-recording config reload, the file name, the indicator dot. Only the Recorder is remote: Start sends the
// RecorderSettings (EyeCapX86Start), SubmitFrame copies the two eyes into a ring slot on the game's GPU and publishes a
// frame descriptor, State / Status mirror what the helper reports.
//
// Start (producer, its link thread): published = 0, fills `start`, full barrier, startSeq + 1 (= the recording's seq),
//   sets Wake. Helper: creates `slots` shared SBS textures + two shared fences (ready, free) on its own D3D11 device on the
//   game's adapter (start.luid), DUPLICATES their NT handles INTO the game process (PROCESS_DUP_HANDLE), starts its
//   Recorder and, once that is Recording (or Failed), writes ringTex / ringReady / ringFree / ackResult (+ status.message on
//   failure) and LAST ackSeq = seq. Producer: opens the handles on the game's device (IFrameSource::OpenRemoteRing, which
//   closes them) and records.
// Frame (producer, game thread, inside xrEndFrame): slot k is reused only when the free fence has completed the frame the
//   slot held (a CPU check, never a GPU wait on the helper); two copies + Signal(ready, v) + Flush on the game's immediate
//   context (D3D11Source::CopyEyes, as for 64-bit games); frames[v % EYECAP_X86_FRAMES] = descriptor, full barrier,
//   published = v, Wake. A busy slot drops the frame (counted, like a full ring in the 64-bit recorder).
//   Helper, in order for every value in (consumed, published]: waits on the CPU until ready has completed v (a dead game
//   cannot hang the helper's GPU), Recorder::SubmitFrame from the slot (one GPU copy into the recorder's own ring), then
//   Signal(free, v) on the same context; consumed = v.
// Stop (producer): stops submitting, waits until the game thread left SubmitFrame, stopSeq = seq, Wake. Helper: takes the
//   frames still published, Recorder::Stop, finalize; the status (seqlock) says recSeq = seq, state Idle / Failed and the
//   "saved <file>" message. Producer: releases its side of the ring.
// Liveness: producerActive = number of armed sessions with a recorder (0 = the helper finishes and exits); producerPid is
//   claimed with InterlockedCompareExchange (0 or a dead pid) -- one 32-bit game per block, a second one is not recorded.
//   helperPid is claimed the same way; helperTick (GetTickCount, every <= 50 ms) older than EYECAP_X86_STALE_MS or a dead
//   helperPid = no helper. The helper also watches the game process handle: a game that exits / crashes mid-recording gets
//   its recording finalized by the helper.
#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EYECAP_X86_SHM_NAME L"Local\\OpenXR-EyeCapture-X86"
#define EYECAP_X86_WAKE_SUFFIX L"-Wake"
#define EYECAP_X86_QUIT_SUFFIX L"-Quit"
#define EYECAP_X86_MUTEX_SUFFIX L"-Recorder"
// Test override of <block> for the layer, the helper and the tray (tests/run_x86.ps1), so a test never meets a real game.
#define EYECAP_X86_TEST_SHM_ENV L"XR_EYE_CAPTURE_TEST_X86_SHM"

enum : uint32_t {
    EYECAP_X86_MAGIC = 0x52363858u,   // 'X86R'
    EYECAP_X86_VERSION = 1,
    EYECAP_X86_MAX_SLOTS = 8,
    EYECAP_X86_FRAMES = 16,           // frame descriptors (> slots: a descriptor is never rewritten before it was taken)
    EYECAP_X86_STALE_MS = 3000        // a helper heartbeat older than this = no helper
};

enum : uint32_t { EYECAP_X86_ACK_OK = 0, EYECAP_X86_ACK_FAILED = 1 };

#pragma pack(push, 8)

// The recording request: RecorderSettings (src/recorder.h) + the ring geometry, written by the producer before startSeq.
struct EyeCapX86Start {
    uint32_t seq;                       // = the startSeq this request belongs to
    uint32_t width, height, leftWidth;  // SBS ring texture (left eye at x = 0, right eye at x = leftWidth)
    uint32_t format;                    // DXGI_FORMAT: the typed copy-family stage format
    uint32_t srgb;                      // 1 = the bytes are sRGB-encoded (gamma override applied)
    uint32_t slots;                     // ring slots (<= EYECAP_X86_MAX_SLOTS)
    uint32_t luidLow;                   // the game device's adapter
    int32_t  luidHigh;
    // RecorderSettings
    uint32_t codec, bitrate, rateControl, qp;
    int32_t  qualityVsSpeed;
    uint32_t constantFrameRate, fps, maxFps, maxWidth, audio;
    float    headSmoothing, headFollowDeg;
    uint32_t rollLock;
    float    fovScale;
    uint32_t edgeGuard, symmetricFov;
    uint32_t domeProjection, domeWidth, domeFill;
    float    domeFadeS, domeFeatherDeg;
    uint32_t youtube, tag3d, layout3d, eyeCrop, startFovValid;
    float    startFov[2][4];
    uint32_t startEyeW[2], startEyeH[2];
    uint32_t domeSharedView;            // was padding (0 from an older layer = off)
    wchar_t  path[520];                 // output .mp4
    wchar_t  audioDevice[256];
};

struct EyeCapX86Eye {
    uint32_t hasPose, x0, w, h;         // the eye's rectangle in the slot: (x0, 0, w, h)
    float    q[4], p[3], fov[4];        // render pose (x y z w / x y z) and fov (left right up down, radians)
    uint32_t pad0;
};

struct EyeCapX86Frame {
    uint32_t value;                     // ready fence value of this frame (1, 2, 3, ... per recording); 0 = empty
    uint32_t seq;                       // the recording (startSeq) it belongs to
    uint32_t slot;
    uint32_t pad0;
    int64_t  ts100ns;                   // presentation time on the QPC clock (the layer's VideoClock; QPC is system-wide)
    int64_t  displayTimeNs;             // XrFrameEndInfo::displayTime (head smoothing / dome dt)
    EyeCapX86Eye eye[2];                // after swap_eyes: eye[0] = the left half of the video
};

// The helper's Recorder, written under statusSeq (seqlock: odd while writing).
struct EyeCapX86Status {
    uint32_t recSeq;                    // the recording these values belong to (0 = none yet)
    uint32_t state;                     // RecState
    uint32_t width, height, fps;
    uint32_t pad0;
    uint64_t framesWritten, framesDropped;
    double   elapsedSec;
    wchar_t  file[260];
    char     message[256];              // UTF-8 ("saved <file>", or why it failed)
};

struct EyeCapX86 {
    uint32_t magic;
    uint32_t version;
    // ---- producer (the game's layer) ----
    volatile LONG producerPid;          // claimed with InterlockedCompareExchange (0 / a dead pid -> own pid)
    volatile LONG producerActive;       // armed sessions with a recorder; 0 = the helper finishes and exits
    volatile LONG producerTick;         // GetTickCount of the producer's last link-thread pass (diagnostic)
    uint32_t producerBits;              // 32 (diagnostic)
    wchar_t  game[64];                  // exe base name
    volatile LONG startSeq;             // bumped after `start` is complete
    volatile LONG stopSeq;              // = the startSeq of the recording to stop (or to cancel before its ack)
    volatile LONG published;            // newest frame value of the current recording (0 = none yet)
    uint32_t pad0;
    EyeCapX86Start start;
    EyeCapX86Frame frames[EYECAP_X86_FRAMES];   // frames[value % EYECAP_X86_FRAMES]
    // ---- helper (EyeCaptureRecorder.exe) ----
    volatile LONG helperPid;            // claimed like producerPid
    volatile LONG helperTick;           // GetTickCount of the helper's last loop pass (heartbeat)
    volatile LONG ackSeq;               // the startSeq answered; written LAST
    uint32_t ackResult;                 // EYECAP_X86_ACK_*
    uint32_t ringCount;
    uint32_t pad1;
    uint64_t ringTex[EYECAP_X86_MAX_SLOTS];   // NT handle values valid IN THE PRODUCER PROCESS (duplicated into it)
    uint64_t ringReady, ringFree;
    volatile LONG consumed;             // newest frame value the helper has taken
    volatile LONG statusSeq;            // seqlock of `status`
    EyeCapX86Status status;
};

#pragma pack(pop)

// The layout is the same in the 32-bit layer and the 64-bit helper.
static_assert(sizeof(EyeCapX86Start) == 1744, "EyeCapX86Start layout");
static_assert(sizeof(EyeCapX86Eye) == 64, "EyeCapX86Eye layout");
static_assert(sizeof(EyeCapX86Frame) == 160 && offsetof(EyeCapX86Frame, ts100ns) == 16, "EyeCapX86Frame layout");
static_assert(sizeof(EyeCapX86Status) == 824 && offsetof(EyeCapX86Status, framesWritten) == 24, "EyeCapX86Status layout");
static_assert(offsetof(EyeCapX86, start) == 168 && offsetof(EyeCapX86, frames) == 1912, "EyeCapX86 layout (producer part)");
static_assert(offsetof(EyeCapX86, helperPid) == 4472 && offsetof(EyeCapX86, ringTex) == 4496, "EyeCapX86 layout (helper part)");
static_assert(offsetof(EyeCapX86, status) == 4584 && sizeof(EyeCapX86) == 5408, "EyeCapX86 layout (status)");

// <block> name: the test override (XR_EYE_CAPTURE_TEST_X86_SHM) or EYECAP_X86_SHM_NAME. Pure; out gets at most cap chars.
inline void EyeCapX86BlockName(wchar_t* out, size_t cap) {
    if (!out || cap == 0) return;
    const DWORD n = GetEnvironmentVariableW(EYECAP_X86_TEST_SHM_ENV, out, (DWORD)cap);
    if (n > 0 && n < cap) return;
    size_t i = 0;
    for (const wchar_t* s = EYECAP_X86_SHM_NAME; s[i] && i + 1 < cap; ++i) out[i] = s[i];
    out[i] = 0;
}

// Consistent copy of `status` (seqlock). false = the writer kept it busy.
inline bool EyeCapX86ReadStatus(const EyeCapX86* p, EyeCapX86Status* out) {
    for (int tries = 0; tries < 100; ++tries) {
        const LONG s1 = p->statusSeq;
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(out, (const void*)&p->status, sizeof(EyeCapX86Status));
        MemoryBarrier();
        if (p->statusSeq == s1) return true;
    }
    return false;
}
inline void EyeCapX86BeginStatus(EyeCapX86* p) { InterlockedIncrement(&p->statusSeq); MemoryBarrier(); }
inline void EyeCapX86EndStatus(EyeCapX86* p) { MemoryBarrier(); InterlockedIncrement(&p->statusSeq); }

// The helper is there: a claimed pid that is alive (the caller checks the process) and a fresh heartbeat.
inline bool EyeCapX86HelperFresh(const EyeCapX86* p, DWORD nowTick) {
    return p->helperPid != 0 && (DWORD)(nowTick - (DWORD)p->helperTick) < (DWORD)EYECAP_X86_STALE_MS;
}
