// recorder_remote -- the Recorder of the 32-bit layer eye_capture32.dll (32-bit OpenXR games, docs/X86_DESIGN.md).
//
// A 32-bit game has 2 GB of address space (Overlord II is not even large-address-aware), so the encoder, Media Foundation,
// WASAPI and the re-projection never run inside it. This file implements the same class as src/recorder.cpp (recorder.h,
// so src/layer.cpp is compiled unchanged on top of it), but every recording is made by the 64-bit helper
// EyeCaptureRecorder.exe (src/recorder_helper.cpp) through the protocol of src/x86_share_proto.h:
//   * constructor / destructor: attach to / release the process-wide block (created here; producerActive counts the
//     recorders, so the helper exits when the last armed session is gone);
//   * Start: a link thread per recording sends the RecorderSettings, waits for the helper's ring, opens it on the game's
//     device (IFrameSource::OpenRemoteRing) and reports Recording; it watches the helper and runs the stop handshake;
//   * SubmitFrame (game thread, inside xrEndFrame): max_fps decimation, a CPU check of the slot's free fence, the two eye
//     copies + Signal(ready) + Flush on the game's immediate context (D3D11Source::CopyEyes, as for 64-bit games), one
//     frame descriptor, published + Wake. No CPU wait and no GPU wait on the helper, ever;
//   * State / Status: the helper's Recorder (seqlock), plus the frames dropped here.
// In the game: three ring textures (opened, not allocated here), two fences, one small block, one thread per recording.
// Overlay layers (record_overlays) are not recorded for 32-bit games (logged once); D3D12 sessions are refused.
#include "recorder.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "capture_common.h"   // ExeBaseName
#include "log.h"
#include "shared_status.h"   // EyeCapProcessAlive, EyeCapCopy
#include "x86_share_proto.h"

static_assert(sizeof(void*) == 4, "recorder_remote.cpp is the 32-bit layer's Recorder (64-bit builds use recorder.cpp)");

namespace {
const UINT kRingSlots = 3;               // like the 64-bit recorder's kRingSize: the game thread never waits
const DWORD kHelperWaitMs = 8000;        // a start waits this long for the helper (the tray starts it within ~2 s)
const DWORD kAckWaitMs = 30000;          // ring + encoder set-up in the helper
const DWORD kFinalWaitMs = 120000;       // finalize in the helper (the layer's own WaitIdle gives up after 30 s)

// Process-wide attachment to the block (one per process: one 32-bit game is recorded at a time).
struct Link {
    std::mutex m;
    int refs = 0;
    HANDLE map = nullptr, wake = nullptr;
    EyeCapX86* blk = nullptr;
    bool owner = false;                  // this process holds producerPid
    std::wstring name;
    DWORD otherPid = 0;                  // another live 32-bit game held the block at attach time
};
Link& g_link = *new Link;   // never destroyed (DLL unload at process exit)

std::string N(const std::wstring& w) { return elog::Narrow(w); }

void Attach() {
    std::lock_guard<std::mutex> lk(g_link.m);
    if (g_link.refs++ > 0) {
        if (g_link.blk && g_link.owner) InterlockedIncrement(&g_link.blk->producerActive);
        return;
    }
    wchar_t name[128];
    EyeCapX86BlockName(name, 128);
    g_link.name = name;
    if (wcscmp(name, EYECAP_X86_SHM_NAME) != 0)
        LOGI("recorder: XR_EYE_CAPTURE_TEST_X86_SHM=%s: private 32-bit recorder block (test)", N(g_link.name).c_str());
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(EyeCapX86), name);
    if (!m) {
        LOGE("recorder: cannot create the 32-bit recorder block %s (error %lu) -- this game cannot be recorded",
             N(g_link.name).c_str(), GetLastError());
        return;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    auto* b = (EyeCapX86*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EyeCapX86));
    if (!b) {
        LOGE("recorder: cannot map the 32-bit recorder block (error %lu)", GetLastError());
        CloseHandle(m);
        return;
    }
    const LONG me = (LONG)GetCurrentProcessId();
    const LONG old = b->producerPid;
    if (existed && b->magic == EYECAP_X86_MAGIC && old && old != me && EyeCapProcessAlive((DWORD)old)) {
        g_link.otherPid = (DWORD)old;
        LOGW("recorder: another 32-bit game (pid %lu) is already recorded through %s -- this one is not (one 32-bit game at a "
             "time)", (unsigned long)old, N(g_link.name).c_str());
    } else if (InterlockedCompareExchange(&b->producerPid, me, old) == old) {
        g_link.owner = true;
        b->magic = EYECAP_X86_MAGIC;
        b->version = EYECAP_X86_VERSION;
        b->producerBits = 32;
        EyeCapCopy(b->game, 64, ExeBaseName().c_str());
        InterlockedExchange(&b->published, 0);
        InterlockedExchange(&b->stopSeq, b->startSeq);   // nothing of a previous game stays pending
        InterlockedExchange(&b->producerActive, 1);
        InterlockedExchange(&b->producerTick, (LONG)GetTickCount());
    } else {
        LOGW("recorder: another 32-bit game took the recorder block at the same moment -- this one is not recorded");
    }
    const std::wstring wake = g_link.name + EYECAP_X86_WAKE_SUFFIX;
    g_link.wake = CreateEventW(nullptr, FALSE, FALSE, wake.c_str());
    g_link.map = m;
    g_link.blk = b;
    if (g_link.owner)
        LOGI("recorder: 32-bit game -- recordings are made by EyeCaptureRecorder.exe (64-bit helper, started by the tray); "
             "block %s %s", N(g_link.name).c_str(), existed ? "(re)claimed" : "created");
}

void Detach() {
    std::lock_guard<std::mutex> lk(g_link.m);
    if (g_link.refs <= 0 || --g_link.refs > 0) {
        if (g_link.refs > 0 && g_link.blk && g_link.owner) InterlockedDecrement(&g_link.blk->producerActive);
        return;
    }
    if (g_link.blk && g_link.owner) {
        InterlockedExchange(&g_link.blk->producerActive, 0);   // the helper finishes and exits
        InterlockedExchange(&g_link.blk->producerPid, 0);
        if (g_link.wake) SetEvent(g_link.wake);
        LOGI("recorder: no armed session left -- the 32-bit recorder block is released (the helper exits)");
    }
    if (g_link.blk) UnmapViewOfFile(g_link.blk);
    if (g_link.map) CloseHandle(g_link.map);
    if (g_link.wake) CloseHandle(g_link.wake);
    g_link.blk = nullptr;
    g_link.map = nullptr;
    g_link.wake = nullptr;
    g_link.owner = false;
    g_link.otherPid = 0;
}
} // namespace

const char* RecStateName(RecState s) {
    switch (s) {
    case RecState::Idle:      return "idle";
    case RecState::Starting:  return "starting";
    case RecState::Recording: return "recording";
    case RecState::Stopping:  return "stopping";
    default:                  return "failed";
    }
}

struct Recorder::Impl {
    Recorder* owner = nullptr;
    std::thread worker;
    // this recording
    IFrameSource* src = nullptr;
    RecorderSettings s;
    UINT W = 0, H = 0, leftW = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool srgb = true;
    LONG seq = 0;                        // startSeq of this recording
    DWORD helperPid = 0;
    std::atomic<bool> stopReq{false};
    std::atomic<int> gameBusy{0};
    // game thread (SubmitFrame)
    ComPtr<ID3D11Fence> freeFence;       // the helper's `free` fence, opened on the game's device
    UINT slots = 0, nextSlot = 0;
    uint64_t slotVal[EYECAP_X86_MAX_SLOTS] = {};
    int64_t nextDue = INT64_MIN, lastTs = INT64_MIN;
    std::atomic<uint64_t> dropBusy{0}, dropGpu{0}, dropDup{0}, decimated{0}, sent{0};
    bool loggedOverlays = false;
    // status (link thread writes, any thread reads)
    std::mutex statusLock;
    RecorderStatus last;                 // the helper's view of this recording (state taken from state_)
    std::string message;                 // our own message (failures before / around the helper)

    void SetMessage(const std::string& m) {
        std::lock_guard<std::mutex> lk(statusLock);
        message = m;
    }
    void Fail(const std::string& m) {
        LOGE("recorder: %s", m.c_str());
        SetMessage(m);
        owner->state_ = RecState::Failed;
    }
    bool HelperAlive(EyeCapX86* b) const {
        return b->helperPid != 0 && EyeCapX86HelperFresh(b, GetTickCount()) && EyeCapProcessAlive((DWORD)b->helperPid);
    }
    // The helper's status for this recording into `last`; true = it is about this recording.
    bool PullStatus(EyeCapX86* b, EyeCapX86Status* out = nullptr) {
        EyeCapX86Status st{};
        if (!EyeCapX86ReadStatus(b, &st) || st.recSeq != (uint32_t)seq) return false;
        st.message[sizeof(st.message) - 1] = 0;
        st.file[259] = 0;
        std::lock_guard<std::mutex> lk(statusLock);
        last.width = st.width;
        last.height = st.height;
        last.fps = st.fps;
        last.framesWritten = st.framesWritten;
        last.framesDropped = st.framesDropped;
        last.elapsedSec = st.elapsedSec;
        if (st.file[0]) last.file = st.file;
        last.message = st.message;
        if (out) *out = st;
        return true;
    }
    void Run();
    void Request(EyeCapX86* b);
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    Attach();
}

Recorder::~Recorder() {
    Stop();
    WaitIdle(30000);
    if (impl_->worker.joinable()) impl_->worker.detach();   // only if the helper hung for 30 s
    Detach();
}

bool Recorder::Start(IFrameSource* src, const RecorderSettings& s, UINT w, UINT h, UINT leftWidth, DXGI_FORMAT fmt, bool srgbEncoded) {
    const RecState st = state_.load();
    if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    Impl& m = *impl_;
    m.src = src;
    m.s = s;
    m.W = w; m.H = h; m.leftW = leftWidth; m.fmt = fmt; m.srgb = srgbEncoded;
    m.stopReq = false;
    m.seq = 0;
    m.helperPid = 0;
    m.freeFence.Reset();
    m.slots = 0; m.nextSlot = 0;
    for (auto& v : m.slotVal) v = 0;
    m.nextDue = INT64_MIN; m.lastTs = INT64_MIN;
    m.dropBusy = 0; m.dropGpu = 0; m.dropDup = 0; m.decimated = 0; m.sent = 0;
    m.loggedOverlays = false;
    {
        std::lock_guard<std::mutex> lk(m.statusLock);
        m.last = RecorderStatus();
        m.last.file = s.path;
        m.last.fps = s.fps;
        m.message.clear();
    }
    state_ = RecState::Starting;
    m.worker = std::thread([this] { impl_->Run(); });
    return true;
}

// Link thread: the request (settings + ring geometry) into the block. g_link.blk is valid while this Recorder exists.
void Recorder::Impl::Request(EyeCapX86* b) {
    EyeCapX86Start& r = b->start;
    EyeCapX86Start q{};
    q.width = W; q.height = H; q.leftWidth = leftW;
    q.format = (uint32_t)fmt;
    q.srgb = srgb ? 1u : 0u;
    q.slots = kRingSlots;
    const LUID luid = src->AdapterLuid();
    q.luidLow = luid.LowPart; q.luidHigh = luid.HighPart;
    q.codec = (uint32_t)s.codec; q.bitrate = s.bitrate; q.rateControl = (uint32_t)s.rateControl; q.qp = s.qp;
    q.qualityVsSpeed = s.qualityVsSpeed;
    q.constantFrameRate = s.constantFrameRate ? 1u : 0u;
    q.fps = s.fps; q.maxFps = s.maxFps; q.maxWidth = s.maxWidth; q.audio = s.audio ? 1u : 0u;
    q.headSmoothing = s.head.smoothing; q.headFollowDeg = s.head.followDeg; q.rollLock = s.head.rollLock ? 1u : 0u;
    q.fovScale = s.head.fovScale; q.edgeGuard = s.head.edgeGuard ? 1u : 0u; q.symmetricFov = s.head.symmetricFov ? 1u : 0u;
    q.domeProjection = (uint32_t)s.dome.projection; q.domeWidth = s.dome.width; q.domeFill = (uint32_t)s.dome.fill;
    q.domeFadeS = s.dome.fadeS; q.domeFeatherDeg = s.dome.featherDeg; q.domeSharedView = s.dome.sharedView ? 1u : 0u;
    q.youtube = s.youtube ? 1u : 0u; q.tag3d = s.tag3d ? 1u : 0u;
    q.layout3d = (uint32_t)s.layout3d; q.eyeCrop = (uint32_t)s.eyeCrop;
    q.startFovValid = s.startFovValid ? 1u : 0u;
    memcpy(q.startFov, s.startFov, sizeof(q.startFov));
    for (int e = 0; e < 2; ++e) { q.startEyeW[e] = s.startEyeW[e]; q.startEyeH[e] = s.startEyeH[e]; }
    EyeCapCopy(q.path, 520, s.path.c_str());
    EyeCapCopy(q.audioDevice, 256, s.audioDevice.c_str());
    InterlockedExchange(&b->published, 0);
    q.seq = (uint32_t)(b->startSeq + 1);
    memcpy((void*)&r, &q, sizeof(q));
    MemoryBarrier();
    seq = InterlockedIncrement(&b->startSeq);   // the only writer of startSeq (the layer thread under the Recorder)
    if ((uint32_t)seq != q.seq) r.seq = (uint32_t)seq;
    MemoryBarrier();
    if (g_link.wake) SetEvent(g_link.wake);
}

void Recorder::Impl::Run() {
    EyeCapX86* b = g_link.blk;
    const std::string file = N(s.path);
    if (!b) { Fail("not started: the 32-bit recorder block could not be created (see the lines above)"); return; }
    if (!g_link.owner) {
        Fail("not started: another 32-bit game" + (g_link.otherPid ? " (pid " + std::to_string(g_link.otherPid) + ")" : std::string()) +
             " is already recorded through EyeCaptureRecorder.exe -- one 32-bit game at a time");
        return;
    }
    if (!src || src->Api() != GfxApi::D3D11) {
        Fail(std::string("not started: 32-bit ") + (src ? GfxApiName(src->Api()) : "unknown") +
             " games are not supported (32-bit games: Direct3D 11 only)");
        return;
    }
    // 1. the helper (the tray starts it when this block appears; a start right after the game started may wait for it)
    {
        const ULONGLONG t0 = GetTickCount64();
        bool logged = false;
        while (!HelperAlive(b)) {
            if (stopReq.load()) { SetMessage("start cancelled"); owner->state_ = RecState::Idle; return; }
            if (GetTickCount64() - t0 >= kHelperWaitMs) {
                Fail("not started: EyeCaptureRecorder.exe (the 64-bit recording helper for 32-bit games) is not running -- the "
                     "tray app starts it when a 32-bit OpenXR game starts (is EyeCaptureTray.exe running? see logs\\tray.log)");
                return;
            }
            if (!logged) { logged = true; LOGI("recorder: waiting for EyeCaptureRecorder.exe (up to %lu s)", kHelperWaitMs / 1000); }
            InterlockedExchange(&b->producerTick, (LONG)GetTickCount());
            Sleep(50);
        }
        helperPid = (DWORD)b->helperPid;
    }
    // 2. the request; 3. the helper's ring + Recorder
    Request(b);
    LOGI("recorder: start #%ld sent to EyeCaptureRecorder.exe (pid %lu): %ux%u SBS (left %u) %s %s, %u slots -> %s", (long)seq,
         (unsigned long)helperPid, W, H, leftW, DxgiFormatName(fmt), srgb ? "sRGB-encoded" : "linear", kRingSlots, file.c_str());
    {
        // A helper that goes away meanwhile (e.g. the one of a previous game, just exiting) is not the end: the request stays
        // in the block and the next helper the tray starts takes it (its first unanswered startSeq).
        const ULONGLONG t0 = GetTickCount64();
        ULONGLONG deadSince = 0;
        for (;;) {
            if (b->ackSeq == seq) break;
            const ULONGLONG now = GetTickCount64();
            if (HelperAlive(b)) deadSince = 0;
            else if (!deadSince) deadSince = now;
            const bool gone = deadSince && now - deadSince >= kHelperWaitMs;
            if (gone || stopReq.load() || now - t0 >= kAckWaitMs) {
                InterlockedExchange(&b->stopSeq, seq);   // cancel: the helper drops / stops this request
                if (g_link.wake) SetEvent(g_link.wake);
                if (stopReq.load() && !gone) { SetMessage("start cancelled"); owner->state_ = RecState::Idle; return; }
                Fail(gone ? "not started: EyeCaptureRecorder.exe went away during the start (and no other one came)"
                          : "not started: EyeCaptureRecorder.exe did not answer within 30 s");
                return;
            }
            InterlockedExchange(&b->producerTick, (LONG)GetTickCount());
            Sleep(10);
        }
        helperPid = (DWORD)b->helperPid;
    }
    MemoryBarrier();
    EyeCapX86Status st{};
    if (b->ackResult != EYECAP_X86_ACK_OK) {
        PullStatus(b, &st);
        Fail(st.message[0] ? std::string(st.message) : std::string("recording failed to start in EyeCaptureRecorder.exe"));
        return;
    }
    // 4. the ring on the game's device (the handles were duplicated into this process; OpenRemoteRing closes them)
    const UINT n = std::min<UINT>(b->ringCount, EYECAP_X86_MAX_SLOTS);
    HANDLE tex[EYECAP_X86_MAX_SLOTS] = {};
    for (UINT i = 0; i < n; ++i) tex[i] = (HANDLE)(uintptr_t)b->ringTex[i];
    const HANDLE ready = (HANDLE)(uintptr_t)b->ringReady, freeH = (HANDLE)(uintptr_t)b->ringFree;
    ComPtr<ID3D11Fence> fence;
    if (n == 0 || !src->OpenRemoteRing(tex, n, ready, freeH, &fence) || !fence) {
        InterlockedExchange(&b->stopSeq, seq);
        if (g_link.wake) SetEvent(g_link.wake);
        Fail("not started: the helper's ring could not be opened on the game's device (see the lines above)");
        return;
    }
    freeFence = fence;
    slots = n;
    PullStatus(b);
    RecState e = RecState::Starting;
    if (!owner->state_.compare_exchange_strong(e, RecState::Recording)) {
        // Stop() arrived meanwhile (state Stopping): the stop handshake below finalizes the (empty) recording
    } else {
        LOGI("recorder: RECORDING -> %s (32-bit game, encoded by EyeCaptureRecorder.exe pid %lu)", file.c_str(), (unsigned long)helperPid);
    }
    // 5. while recording: the helper's status, its liveness, a stop of either side
    bool helperDied = false;
    for (;;) {
        if (stopReq.load()) break;
        if (!HelperAlive(b)) { helperDied = true; break; }
        if (PullStatus(b, &st)) {
            const RecState hs = (RecState)st.state;
            if (hs == RecState::Stopping || hs == RecState::Idle || hs == RecState::Failed) {
                LOGI("recorder: EyeCaptureRecorder.exe ended the recording itself (%s)%s%s", RecStateName(hs),
                     st.message[0] ? ": " : "", st.message);
                break;
            }
        }
        InterlockedExchange(&b->producerTick, (LONG)GetTickCount());
        Sleep(50);
    }
    // 6. stop: no more frames from the game thread, then the helper's finalize
    owner->state_ = RecState::Stopping;
    {
        const ULONGLONG t = GetTickCount64();
        while (gameBusy.load() > 0 && GetTickCount64() - t < 2000) Sleep(1);
    }
    InterlockedExchange(&b->stopSeq, seq);
    if (g_link.wake) SetEvent(g_link.wake);
    bool final = false;
    if (!helperDied) {
        const ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < kFinalWaitMs) {
            if (PullStatus(b, &st) && ((RecState)st.state == RecState::Idle || (RecState)st.state == RecState::Failed)) {
                final = true;
                break;
            }
            if (!HelperAlive(b)) { helperDied = true; break; }
            InterlockedExchange(&b->producerTick, (LONG)GetTickCount());
            Sleep(20);
        }
    }
    // 7. our side of the ring (waits for our last copy on the game's GPU)
    if (src) src->DestroyRing();
    freeFence.Reset();
    const uint64_t localDrops = dropBusy.load() + dropGpu.load();
    LOGI("recorder: 32-bit game side: %llu frames sent to the helper, dropped here %llu (slot still in use by the helper %llu, "
         "gpu %llu), duplicates %llu, over max_fps %llu", (unsigned long long)sent.load(), (unsigned long long)localDrops,
         (unsigned long long)dropBusy.load(), (unsigned long long)dropGpu.load(), (unsigned long long)dropDup.load(),
         (unsigned long long)decimated.load());
    if (helperDied) {
        Fail("recording lost: EyeCaptureRecorder.exe (pid " + std::to_string(helperPid) + ") stopped during the recording -- " +
             file + " is incomplete");
        return;
    }
    if (!final) {
        Fail("EyeCaptureRecorder.exe did not finish " + file + " within 120 s");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(statusLock);
        message.clear();   // the helper's message ("saved <file>" or why it failed) is the result
        last.framesDropped += localDrops;
    }
    owner->state_ = (RecState)st.state == RecState::Failed ? RecState::Failed : RecState::Idle;
}

bool Recorder::SubmitFrame(int64_t ts, const EyeView& L, const EyeView& R, int64_t displayTimeNs, const OverlayFrame* ov) {
    Impl& m = *impl_;
    m.gameBusy.fetch_add(1);
    struct Guard { std::atomic<int>& c; ~Guard() { c.fetch_sub(1); } } guard{m.gameBusy};
    if (state_.load() != RecState::Recording) return false;
    EyeCapX86* b = g_link.blk;
    if (!b || !m.freeFence || !m.slots) return false;
    if (ov && ov->n && !m.loggedOverlays) {
        m.loggedOverlays = true;
        LOGI("overlay: 32-bit game -- overlay layers (panels / HUD quads) are not recorded, only the projection layer's eyes");
    }
    if (m.lastTs != INT64_MIN && ts <= m.lastTs) { ++m.dropDup; return false; }
    if (m.s.maxFps) {   // the recorder's max_fps decimation, here so a decimated frame costs no copy on the game's GPU
        const int64_t period = 10000000LL / m.s.maxFps;
        if (m.nextDue != INT64_MIN && ts < m.nextDue - period / 4) { ++m.decimated; return false; }
        m.nextDue = (m.nextDue == INT64_MIN || ts > m.nextDue + period) ? ts + period : m.nextDue + period;
    }
    const UINT slot = m.nextSlot;
    const uint64_t prev = m.slotVal[slot];
    if (prev && m.freeFence->GetCompletedValue() < prev) { ++m.dropBusy; return false; }   // the helper still reads it
    EyeView l = L, r = R;
    l.w = std::min<uint32_t>(l.w, m.leftW);
    l.h = std::min<uint32_t>(l.h, m.H);
    r.w = std::min<uint32_t>(r.w, m.W - m.leftW);
    r.h = std::min<uint32_t>(r.h, m.H);
    const uint64_t v = m.src->CopyEyes(slot, l, r, m.leftW, 0);
    if (!v) { ++m.dropGpu; return false; }
    m.slotVal[slot] = v;
    m.nextSlot = (slot + 1) % m.slots;
    m.lastTs = ts;
    EyeCapX86Frame& f = b->frames[v % EYECAP_X86_FRAMES];
    EyeCapX86Frame d{};
    d.value = (uint32_t)v;
    d.seq = (uint32_t)m.seq;
    d.slot = slot;
    d.ts100ns = ts;
    d.displayTimeNs = displayTimeNs ? displayTimeNs : ts * 100;
    const EyeView* ev[2] = {&l, &r};
    const UINT x0[2] = {0, m.leftW};
    for (int e = 0; e < 2; ++e) {
        EyeCapX86Eye& o = d.eye[e];
        const EyeView& vw = *ev[e];
        o.hasPose = vw.hasPose ? 1u : 0u;
        o.x0 = x0[e]; o.w = vw.w; o.h = vw.h;
        memcpy(o.q, vw.orientation, sizeof(o.q));
        memcpy(o.p, vw.position, sizeof(o.p));
        memcpy(o.fov, vw.fov, sizeof(o.fov));
    }
    memcpy((void*)&f, &d, sizeof(d));
    MemoryBarrier();
    InterlockedExchange(&b->published, (LONG)v);
    if (g_link.wake) SetEvent(g_link.wake);
    ++m.sent;
    return true;
}

void Recorder::Stop() {
    Impl& m = *impl_;
    m.stopReq = true;
    RecState e = RecState::Recording;
    if (!state_.compare_exchange_strong(e, RecState::Stopping)) {
        e = RecState::Starting;
        state_.compare_exchange_strong(e, RecState::Stopping);
    }
}

bool Recorder::WaitIdle(DWORD ms) {
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const RecState st = state_.load();
        if (st == RecState::Idle || st == RecState::Failed) break;
        if (GetTickCount64() - t0 >= ms) return false;
        Sleep(5);
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    return true;
}

RecorderStatus Recorder::Status() {
    Impl& m = *impl_;
    RecorderStatus st;
    {
        std::lock_guard<std::mutex> lk(m.statusLock);
        st = m.last;
        if (!m.message.empty()) st.message = m.message;
    }
    st.state = state_.load();
    if (st.state == RecState::Recording || st.state == RecState::Stopping) st.framesDropped += m.dropBusy.load() + m.dropGpu.load();
    if (st.file.empty()) st.file = m.s.path;
    return st;
}
