// EyeCaptureRecorder.exe -- the 64-bit recording helper for 32-bit OpenXR games (docs/X86_DESIGN.md).
//
// The 32-bit layer eye_capture32.dll keeps the game-side work small (control thread, two GPU copies per frame) and hands
// each recording to this process through the block of src/x86_share_proto.h; here the normal Recorder (src/recorder.h:
// re-projection / symmetric_fov / head smoothing / dome, the hardware encoder, WASAPI audio, st3d, the sidecar) makes the
// same MP4 a 64-bit game would get. The tray's post-processing (MKV, 8K, flat) works on it unchanged.
//
//   * Started by the tray (tray.cpp X86Tick) when the block exists and its producer (the game) is alive and active, with
//     --home <tray folder> (logs). Exit codes: 0 done (the game's session ended / the game exited / quit event / no
//     block within 10 s), 1 failed (cannot open the game process / block), 3 another EyeCaptureRecorder runs (<block>-Recorder).
//   * One D3D11 device on the game's adapter (EyeCapX86Start::luid) = the "game device" of a D3D11 frame source: per
//     recording a ring of shared SBS textures + shared ready / free fences is created on it and DUPLICATED INTO the game
//     process; each published frame is waited for on the CPU (ready), copied into the Recorder (SubmitFrame = one GPU copy
//     on this device) and released (Signal free). A frame the game's GPU has not finished within 500 ms is skipped.
//   * The RecorderSettings come from the game (its config reload at the recording start), so the result is identical to a
//     64-bit game's recording. This process reads config.ini only for log_level.
//   * The game exits or crashes while recording: the helper finalizes the MP4 and hands "saved <file>" to the tray's status
//     block itself (the game's control thread is gone); a normal end is reported by the game (ControlPublishFinal).
// Logs to <home>\logs\eye_capture.log (the layer's log; pid on every line) and to <output folder>\eye_capture.log per
// recording, like the layer.
#include <windows.h>
#include <d3d11_4.h>
#include <shlobj.h>
#include <atomic>
#include <memory>
#include <string>

#include "capture_common.h"
#include "config.h"
#include "frame_source.h"
#include "log.h"
#include "recorder.h"
#include "shared_status.h"
#include "x86_share_proto.h"

namespace {

const int kExitFailed = 1;
const int kExitBusy = 3;           // another EyeCaptureRecorder holds <block>-Recorder (tray.cpp kX86ExitBusy)
const DWORD kNoBlockWaitMs = 10000;
const DWORD kReadyWaitMs = 500;    // CPU wait for the game's copy of one frame

std::string N(const std::wstring& w) { return elog::Narrow(w); }

std::wstring ExeDir() {
    wchar_t p[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::wstring ArgValue(const std::wstring& args, const wchar_t* key) {
    const size_t k = args.find(key);
    if (k == std::wstring::npos) return L"";
    std::wstring h = args.substr(k + wcslen(key));
    while (!h.empty() && h.front() == L' ') h.erase(0, 1);
    if (!h.empty() && h.front() == L'"') {
        const size_t q = h.find(L'"', 1);
        h = h.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
    } else {
        const size_t sp = h.find(L' ');
        if (sp != std::wstring::npos) h = h.substr(0, sp);
    }
    while (!h.empty() && (h.back() == L'\\' || h.back() == L'/')) h.pop_back();
    return h;
}

enum class Phase { Idle, Starting, Recording, Stopping };

struct Helper {
    std::wstring home, blockName, logPath;
    Config cfg;
    EyeCapX86* b = nullptr;
    HANDLE map = nullptr, wake = nullptr, quit = nullptr, game = nullptr;
    DWORD gamePid = 0;
    std::wstring gameName;
    // device + frame source (created at the first start, re-created for another adapter)
    LUID luid{};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext4> ctx4;
    std::string adapter;
    std::unique_ptr<IFrameSource> src;
    std::unique_ptr<Recorder> rec;
    // the current recording
    Phase phase = Phase::Idle;
    LONG handledStart = 0;     // the last startSeq taken (answered or dropped)
    LONG seq = 0;              // the recording in progress
    std::vector<ComPtr<ID3D11Texture2D>> ring;
    ComPtr<ID3D11Fence> ready, free;
    HANDLE readyEvt = nullptr;
    SwapchainInfo info;        // the ring texture as a "swapchain" for the frame source
    UINT W = 0, H = 0, leftW = 0;
    LONG consumed = 0;
    uint64_t taken = 0, skippedNotReady = 0, skippedBad = 0, submitted = 0;
    ULONGLONG lastStatus = 0;
    RecState lastPubState = RecState::Idle;
    bool ackPending = false;
};
Helper& g = *new Helper;   // never destroyed (worker threads of the Recorder may outlive main's locals)

void PublishStatus(bool force) {
    if (!g.b || !g.rec) return;
    const ULONGLONG now = GetTickCount64();
    const RecorderStatus rs = g.rec->Status();
    if (!force && rs.state == g.lastPubState && now - g.lastStatus < 100) return;
    g.lastStatus = now;
    g.lastPubState = rs.state;
    EyeCapX86BeginStatus(g.b);
    EyeCapX86Status& s = g.b->status;
    s.recSeq = (uint32_t)g.seq;
    s.state = (uint32_t)rs.state;
    s.width = rs.width; s.height = rs.height; s.fps = rs.fps;
    s.framesWritten = rs.framesWritten;
    s.framesDropped = rs.framesDropped;
    s.elapsedSec = rs.elapsedSec;
    EyeCapCopy(s.file, 260, rs.file.c_str());
    const size_t n = rs.message.size() < sizeof(s.message) - 1 ? rs.message.size() : sizeof(s.message) - 1;
    memcpy(s.message, rs.message.data(), n);
    s.message[n] = 0;
    EyeCapX86EndStatus(g.b);
}

// A failed start: the message in the status (recSeq = this start), then the ack.
void AckFailed(LONG startSeq, const std::string& why) {
    LOGE("helper: start #%ld failed: %s", (long)startSeq, why.c_str());
    EyeCapX86BeginStatus(g.b);
    EyeCapX86Status& s = g.b->status;
    memset(&s, 0, sizeof(s));
    s.recSeq = (uint32_t)startSeq;
    s.state = (uint32_t)RecState::Failed;
    const size_t n = why.size() < sizeof(s.message) - 1 ? why.size() : sizeof(s.message) - 1;
    memcpy(s.message, why.data(), n);
    EyeCapX86EndStatus(g.b);
    g.b->ackResult = EYECAP_X86_ACK_FAILED;
    g.b->ringCount = 0;
    MemoryBarrier();
    InterlockedExchange(&g.b->ackSeq, startSeq);
}

bool EnsureDevice(LUID l) {
    if (g.dev && g.luid.LowPart == l.LowPart && g.luid.HighPart == l.HighPart) return true;
    if (g.rec && g.rec->State() != RecState::Idle && g.rec->State() != RecState::Failed) return false;
    g.rec.reset();
    g.src.reset();
    g.ctx4.Reset(); g.ctx.Reset(); g.dev5.Reset(); g.dev.Reset();
    g.dev = CreateEncoderDevice(l, &g.adapter);
    if (!g.dev) return false;
    g.dev->GetImmediateContext(&g.ctx);
    if (FAILED(g.dev.As(&g.dev5)) || FAILED(g.ctx.As(&g.ctx4))) { g.dev.Reset(); return false; }
    g.src = CreateD3D11FrameSource(g.dev.Get());
    if (!g.src) { g.dev.Reset(); return false; }
    g.rec = std::make_unique<Recorder>();
    g.luid = l;
    LOGI("helper: D3D11 device on the game's adapter '%s' (LUID %08lx:%08lx)", g.adapter.c_str(), (unsigned long)l.HighPart,
         (unsigned long)l.LowPart);
    return true;
}

void ReleaseRing() {
    g.ring.clear();
    g.ready.Reset();
    g.free.Reset();
}

// One NT handle of ours -> a handle value in the game process (DuplicateHandle; ours is closed).
bool ToGame(HANDLE mine, uint64_t& out) {
    HANDLE theirs = nullptr;
    const BOOL ok = DuplicateHandle(GetCurrentProcess(), mine, g.game, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS);
    const DWORD err = GetLastError();
    CloseHandle(mine);
    if (!ok) { LOGE("helper: DuplicateHandle into the game (pid %lu) failed (error %lu)", (unsigned long)g.gamePid, err); return false; }
    out = (uint64_t)(uintptr_t)theirs;
    return true;
}

// A new start request (startSeq > handledStart). Creates the ring, hands it to the game, starts the Recorder.
void TakeStart(LONG startSeq) {
    g.handledStart = startSeq;
    if (g.b->stopSeq >= startSeq) { LOGI("helper: start #%ld was cancelled by the game before it was taken", (long)startSeq); return; }
    EyeCapX86Start q{};
    MemoryBarrier();
    memcpy(&q, (const void*)&g.b->start, sizeof(q));
    if ((LONG)q.seq != startSeq) LOGW("helper: start #%ld carries seq %u (taken anyway)", (long)startSeq, q.seq);
    q.path[519] = 0;
    q.audioDevice[255] = 0;
    if (g.phase != Phase::Idle) { AckFailed(startSeq, "not started: the previous recording is still finalizing"); return; }
    const DXGI_FORMAT fmt = (DXGI_FORMAT)q.format;
    const UINT slots = q.slots < 1 ? 1 : q.slots > EYECAP_X86_MAX_SLOTS ? EYECAP_X86_MAX_SLOTS : q.slots;
    if (!q.width || !q.height || q.leftWidth > q.width || q.width > 16384 || q.height > 16384 || StageFormatForDxgi(fmt) == DXGI_FORMAT_UNKNOWN) {
        AckFailed(startSeq, "not started: bad ring geometry from the game (" + std::to_string(q.width) + "x" + std::to_string(q.height) +
                            " " + DxgiFormatName(fmt) + ")");
        return;
    }
    LUID l{};
    l.LowPart = q.luidLow;
    l.HighPart = q.luidHigh;
    if (!EnsureDevice(l)) { AckFailed(startSeq, "not started: no D3D11 device on the game's adapter in EyeCaptureRecorder.exe"); return; }
    // the ring: shared SBS textures + the two fences, on our device; NT handles duplicated into the game
    ReleaseRing();
    uint64_t texGame[EYECAP_X86_MAX_SLOTS] = {}, readyGame = 0, freeGame = 0;
    for (UINT i = 0; i < slots; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = q.width; d.Height = q.height; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> t;
        HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &t);
        ComPtr<IDXGIResource1> r1;
        HANDLE h = nullptr;
        if (SUCCEEDED(hr)) hr = t.As(&r1);
        if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
        if (FAILED(hr) || !ToGame(h, texGame[i])) {
            ReleaseRing();
            AckFailed(startSeq, "not started: ring texture " + std::to_string(i) + " (" + std::to_string(q.width) + "x" +
                                std::to_string(q.height) + " " + DxgiFormatName(fmt) + ") could not be shared with the game");
            return;
        }
        g.ring.push_back(t);
    }
    HRESULT hr = g.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.ready));
    if (SUCCEEDED(hr)) hr = g.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.free));
    HANDLE hr1 = nullptr, hf1 = nullptr;
    if (SUCCEEDED(hr)) hr = g.ready->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hr1);
    if (SUCCEEDED(hr)) hr = g.free->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hf1);
    const bool fencesOk = SUCCEEDED(hr) && ToGame(hr1, readyGame) && ToGame(hf1, freeGame);
    if (!fencesOk) {
        if (FAILED(hr)) { if (hr1) CloseHandle(hr1); if (hf1) CloseHandle(hf1); }
        ReleaseRing();
        AckFailed(startSeq, "not started: the ring fences could not be shared with the game");
        return;
    }
    for (UINT i = 0; i < slots; ++i) g.b->ringTex[i] = texGame[i];
    for (UINT i = slots; i < EYECAP_X86_MAX_SLOTS; ++i) g.b->ringTex[i] = 0;
    g.b->ringReady = readyGame;
    g.b->ringFree = freeGame;
    g.b->ringCount = slots;
    // the frame source sees the ring texture as a swapchain image
    g.info = SwapchainInfo();
    g.info.format = (int64_t)fmt;
    g.info.width = q.width;
    g.info.height = q.height;
    g.W = q.width; g.H = q.height; g.leftW = q.leftWidth;
    // RecorderSettings exactly as the game's layer filled them
    RecorderSettings rs;
    rs.path = q.path;
    rs.codec = (VideoCodec)q.codec;
    rs.bitrate = q.bitrate;
    rs.rateControl = (RateControl)q.rateControl;
    rs.qp = q.qp;
    rs.qualityVsSpeed = q.qualityVsSpeed;
    rs.constantFrameRate = q.constantFrameRate != 0;
    rs.fps = q.fps;
    rs.maxFps = q.maxFps;
    rs.maxWidth = q.maxWidth;
    rs.audio = q.audio != 0;
    rs.audioDevice = q.audioDevice;
    rs.head.smoothing = q.headSmoothing;
    rs.head.followDeg = q.headFollowDeg;
    rs.head.rollLock = q.rollLock != 0;
    rs.head.fovScale = q.fovScale;
    rs.head.edgeGuard = q.edgeGuard != 0;
    rs.head.symmetricFov = q.symmetricFov != 0;
    rs.dome.projection = (DomeProjection)q.domeProjection;
    rs.dome.width = q.domeWidth;
    rs.dome.fill = (DomeFill)q.domeFill;
    rs.dome.fadeS = q.domeFadeS;
    rs.dome.featherDeg = q.domeFeatherDeg;
    rs.dome.sharedView = q.domeSharedView != 0;
    rs.youtube = q.youtube != 0;
    rs.tag3d = q.tag3d != 0;
    rs.layout3d = (Layout3d)q.layout3d;
    rs.eyeCrop = (EyeCrop)q.eyeCrop;
    rs.startFovValid = q.startFovValid != 0;
    memcpy(rs.startFov, q.startFov, sizeof(rs.startFov));
    for (int e = 0; e < 2; ++e) { rs.startEyeW[e] = q.startEyeW[e]; rs.startEyeH[e] = q.startEyeH[e]; }
    // the per-recording log mirror next to the recordings, like the layer's
    const size_t slash = rs.path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring dir = rs.path.substr(0, slash);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        elog::SetMirror(dir + L"\\eye_capture.log");
    }
    g.seq = startSeq;
    g.consumed = 0;
    g.taken = g.skippedNotReady = g.skippedBad = g.submitted = 0;
    LOGI("helper: start #%ld for %s (pid %lu): %ux%u SBS (left %u) %s %s, %u shared slots on '%s' -> %s", (long)startSeq,
         N(g.gameName).c_str(), (unsigned long)g.gamePid, q.width, q.height, q.leftWidth, DxgiFormatName(fmt),
         q.srgb ? "sRGB-encoded" : "linear", slots, g.adapter.c_str(), N(rs.path).c_str());
    if (!g.rec->Start(g.src.get(), rs, q.width, q.height, q.leftWidth, fmt, q.srgb != 0)) {
        ReleaseRing();
        AckFailed(startSeq, "not started: the previous recording is still finalizing");
        return;
    }
    g.phase = Phase::Starting;
    g.ackPending = true;
    PublishStatus(true);
}

// Phase Starting: the Recorder left Starting -> the ack (the game opens the ring once it reads ackSeq).
void CheckStarted() {
    const RecState st = g.rec->State();
    if (st == RecState::Starting) return;
    PublishStatus(true);
    if (st == RecState::Recording) {
        g.b->ackResult = EYECAP_X86_ACK_OK;
        MemoryBarrier();
        InterlockedExchange(&g.b->ackSeq, g.seq);
        g.ackPending = false;
        g.phase = Phase::Recording;
        LOGI("helper: recording #%ld -- ring handed to the game", (long)g.seq);
        return;
    }
    // Failed (message from the Recorder) or stopped before it ran
    const RecorderStatus rs = g.rec->Status();
    ReleaseRing();
    g.phase = Phase::Idle;
    g.ackPending = false;
    AckFailed(g.seq, rs.message.empty() ? std::string("recording failed to start") : rs.message);
}

// Phase Recording: every published frame, in order.
void TakeFrames() {
    const LONG pub = g.b->published;
    while (g.consumed < pub) {
        const LONG v = g.consumed + 1;
        EyeCapX86Frame f{};
        MemoryBarrier();
        memcpy(&f, (const void*)&g.b->frames[(uint32_t)v % EYECAP_X86_FRAMES], sizeof(f));
        g.consumed = v;
        ++g.taken;
        if (f.value != (uint32_t)v || f.seq != (uint32_t)g.seq || f.slot >= g.ring.size()) {
            if (++g.skippedBad <= 5)
                LOGW("helper: frame %ld: descriptor value %u seq %u slot %u does not match (recording #%ld) -- skipped", (long)v,
                     f.value, f.seq, f.slot, (long)g.seq);
            if (g.free) { g.ctx4->Signal(g.free.Get(), (UINT64)v); g.ctx->Flush(); }
            InterlockedExchange(&g.b->consumed, v);
            continue;
        }
        // the game's copy into the slot must be done: a CPU wait (never a GPU wait on another process)
        bool ok = g.ready->GetCompletedValue() >= (UINT64)v;
        if (!ok && g.readyEvt && SUCCEEDED(g.ready->SetEventOnCompletion((UINT64)v, g.readyEvt)))
            ok = WaitForSingleObject(g.readyEvt, kReadyWaitMs) == WAIT_OBJECT_0;
        if (!ok) {
            if (++g.skippedNotReady <= 5) LOGW("helper: frame %ld: the game's copy did not complete within %lu ms -- skipped", (long)v, kReadyWaitMs);
        } else if (g.rec->State() == RecState::Recording) {
            EyeView ev[2];
            for (int e = 0; e < 2; ++e) {
                const EyeCapX86Eye& s = f.eye[e];
                EyeView& o = ev[e];
                o.image = g.ring[f.slot].Get();
                o.sc = &g.info;
                o.swapchainKey = g.ring[f.slot].Get();
                o.arrayIndex = 0;
                o.x = (int32_t)s.x0;
                o.y = 0;
                o.w = s.w;
                o.h = s.h;
                o.hasPose = s.hasPose != 0;
                memcpy(o.orientation, s.q, sizeof(o.orientation));
                memcpy(o.position, s.p, sizeof(o.position));
                memcpy(o.fov, s.fov, sizeof(o.fov));
            }
            if (g.rec->SubmitFrame(f.ts100ns, ev[0], ev[1], f.displayTimeNs, nullptr)) ++g.submitted;
        }
        g.ctx4->Signal(g.free.Get(), (UINT64)v);   // after the Recorder's copy on this context: the game may reuse the slot
        g.ctx->Flush();
        InterlockedExchange(&g.b->consumed, v);
    }
}

void LogFrames() {
    LOGI("helper: recording #%ld: %llu frames taken from the game, %llu into the recorder, %llu skipped (game copy not done in "
         "time), %llu bad descriptors", (long)g.seq, (unsigned long long)g.taken, (unsigned long long)g.submitted,
         (unsigned long long)g.skippedNotReady, (unsigned long long)g.skippedBad);
}

// The game's tray status block, when the game itself can no longer publish (it exited during the recording): the
// "saved <file>" result for the tray's conversion (the same fields ControlPublishFinal writes). Never over a live owner.
void PublishFinalToTray(const RecorderStatus& rs, const std::wstring& outputDir) {
    if (EnvW(L"XR_EYE_CAPTURE_TRAY_IPC") == L"0" || rs.message.empty()) return;
    HANDLE m = nullptr;
    const std::wstring testShm = EnvW(L"XR_EYE_CAPTURE_TEST_SHM");
    EyeCapShared* shm = testShm.empty() ? EyeCapOpenShared(&m) : EyeCapOpenShared(&m, testShm.c_str());
    if (!shm) return;
    const DWORD cur = shm->ownerPid;
    if (cur == 0 || cur == g.gamePid || !EyeCapProcessAlive(cur)) {
        EyeCapBeginWrite(shm);
        if (cur == g.gamePid) { shm->ownerPid = 0; shm->state = ECS_NONE; }
        shm->width = rs.width;
        shm->height = rs.height;
        shm->fps = rs.fps;
        shm->framesWritten = rs.framesWritten;
        shm->framesDropped = rs.framesDropped;
        EyeCapCopy(shm->file, 260, rs.file.c_str());
        if (!outputDir.empty()) EyeCapCopy(shm->outputDir, 260, outputDir.c_str());
        EyeCapCopy(shm->message, 160, elog::Widen(rs.message).c_str());
        EyeCapEndWrite(shm);
        LOGI("helper: published the final status to the tray (the game is gone): %s", rs.message.c_str());
    }
    UnmapViewOfFile(shm);
    CloseHandle(m);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring args = cmdLine ? cmdLine : L"";
    wchar_t bn[128];
    EyeCapX86BlockName(bn, 128);
    g.blockName = bn;
    HANDLE single = CreateMutexW(nullptr, TRUE, (g.blockName + EYECAP_X86_MUTEX_SUFFIX).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) return kExitBusy;
    g.home = ArgValue(args, L"--home ");
    if (g.home.empty()) g.home = ExeDir();
    g.logPath = LogsDir(g.home) + L"\\eye_capture.log";
    elog::Init(g.logPath, false);
    LoadConfig(g.home, g.cfg);
    const CaptureEnv env = ReadCaptureEnv();
    if (!env.configOverrides.empty()) ApplyConfigOverrides(env.configOverrides, g.cfg);
    elog::SetLevel(g.cfg.logLevel);
    LOGI("==== VR EyeCapture recording helper for 32-bit games (EyeCaptureRecorder) v%s (pid %lu), home %s, block %s",
         EYE_CAPTURE_VERSION, (unsigned long)GetCurrentProcessId(), N(g.home).c_str(), N(g.blockName).c_str());
    g.quit = CreateEventW(nullptr, TRUE, FALSE, (g.blockName + EYECAP_X86_QUIT_SUFFIX).c_str());
    auto quitSet = [] { return g.quit && WaitForSingleObject(g.quit, 0) == WAIT_OBJECT_0; };

    // the block (created by the game's layer); the tray starts us when it exists, a test may start us first
    {
        const ULONGLONG t0 = GetTickCount64();
        for (;;) {
            g.map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g.blockName.c_str());
            if (g.map) break;
            const DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                LOGE("helper: %s exists but may not be opened (access denied): the game runs as administrator -- run the game "
                     "normally, or start the tray app as administrator too", N(g.blockName).c_str());
                return kExitFailed;
            }
            if (quitSet() || GetTickCount64() - t0 >= kNoBlockWaitMs) {
                LOGI("helper: no 32-bit game (no %s) -- exiting", N(g.blockName).c_str());
                return 0;
            }
            Sleep(250);
        }
    }
    g.b = (EyeCapX86*)MapViewOfFile(g.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EyeCapX86));
    if (!g.b || g.b->magic != EYECAP_X86_MAGIC || g.b->version != EYECAP_X86_VERSION) {
        LOGE("helper: %s is not a version %u block (magic 0x%08x, version %u) -- exiting", N(g.blockName).c_str(),
             (unsigned)EYECAP_X86_VERSION, g.b ? g.b->magic : 0u, g.b ? g.b->version : 0u);
        return kExitFailed;
    }
    g.gamePid = (DWORD)g.b->producerPid;
    wchar_t gameName[65] = {};
    memcpy(gameName, (const void*)g.b->game, sizeof(g.b->game));
    g.gameName = gameName;
    if (!g.gamePid || !g.b->producerActive) {
        LOGI("helper: the block has no active 32-bit game (pid %lu, active %ld) -- exiting", (unsigned long)g.gamePid,
             (long)g.b->producerActive);
        return 0;
    }
    g.game = OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g.gamePid);
    if (!g.game) {
        LOGE("helper: cannot open the game process %s (pid %lu, error %lu)%s -- exiting", N(g.gameName).c_str(),
             (unsigned long)g.gamePid, GetLastError(),
             GetLastError() == ERROR_ACCESS_DENIED ? ": it runs as administrator; run it normally, or the tray app as administrator too" : "");
        return kExitFailed;
    }
    {   // claim the helper role (0, or a dead / stale helper)
        const LONG me = (LONG)GetCurrentProcessId();
        const LONG old = g.b->helperPid;
        if (old && old != me && EyeCapX86HelperFresh(g.b, GetTickCount()) && EyeCapProcessAlive((DWORD)old)) {
            LOGI("helper: another EyeCaptureRecorder (pid %ld) serves this game -- exiting", (long)old);
            return kExitBusy;
        }
        InterlockedExchange(&g.b->helperTick, (LONG)GetTickCount());
        if (InterlockedCompareExchange(&g.b->helperPid, me, old) != old) {
            LOGI("helper: another EyeCaptureRecorder claimed this game at the same moment -- exiting");
            return kExitBusy;
        }
    }
    g.wake = CreateEventW(nullptr, FALSE, FALSE, (g.blockName + EYECAP_X86_WAKE_SUFFIX).c_str());
    g.readyEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // starts that were already taken (by an earlier helper) are not ours to answer
    g.handledStart = g.b->ackSeq;
    LOGI("helper: serving %s (pid %lu, 32-bit OpenXR game) -- recordings start / stop with the game's hotkey / tray commands",
         N(g.gameName).c_str(), (unsigned long)g.gamePid);

    std::string why;
    for (;;) {
        HANDLE hs[3] = {g.wake, g.game, g.quit};
        const DWORD w = WaitForMultipleObjects(g.quit ? 3 : 2, hs, FALSE, 50);
        InterlockedExchange(&g.b->helperTick, (LONG)GetTickCount());
        if ((DWORD)g.b->helperPid != GetCurrentProcessId()) { why = "another helper took over"; break; }
        if (w == WAIT_OBJECT_0 + 1) { why = "the game exited"; break; }
        if (w == WAIT_OBJECT_0 + 2) { why = "quit requested by the tray"; break; }
        if ((DWORD)g.b->producerPid != g.gamePid || g.b->producerActive <= 0) { why = "the game's VR session ended"; break; }
        const LONG startSeq = g.b->startSeq;
        if (startSeq > g.handledStart && g.phase == Phase::Idle) TakeStart(startSeq);
        if (g.phase == Phase::Starting) {
            if (g.b->stopSeq >= g.seq) {   // cancelled before the ack: stop our recorder, no ack (the game gave up)
                LOGI("helper: start #%ld cancelled by the game", (long)g.seq);
                g.rec->Stop();
                g.phase = Phase::Stopping;
                g.ackPending = false;
            } else {
                CheckStarted();
            }
        }
        if (g.phase == Phase::Recording) {
            TakeFrames();
            const RecState st = g.rec->State();
            if (g.b->stopSeq >= g.seq) {
                TakeFrames();   // everything the game published before its stop
                LOGI("helper: stop #%ld from the game", (long)g.seq);
                g.rec->Stop();
                g.phase = Phase::Stopping;
            } else if (st != RecState::Recording) {   // stopped itself (disk almost full, encoder failure)
                g.phase = Phase::Stopping;
            }
        }
        if (g.phase == Phase::Stopping) {
            const RecState st = g.rec->State();
            if (st == RecState::Idle || st == RecState::Failed) {
                g.rec->WaitIdle(1000);
                LogFrames();
                ReleaseRing();
                g.phase = Phase::Idle;
                PublishStatus(true);
            }
        }
        if (g.rec && g.phase != Phase::Idle) PublishStatus(false);   // Idle: the last status (final / failed start) stays
    }
    LOGI("helper: finishing -- %s", why.c_str());
    const bool gameGone = WaitForSingleObject(g.game, 0) == WAIT_OBJECT_0;
    if (g.rec && g.phase != Phase::Idle) {
        if (g.phase == Phase::Recording) TakeFrames();
        const RecState st = g.rec->State();
        if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) {
            LOGI("helper: finalizing the recording (%s)", why.c_str());
            g.rec->Stop();
            if (!g.rec->WaitIdle(30000)) LOGE("helper: finalize did not finish within 30 s");
        }
        LogFrames();
        PublishStatus(true);
        const RecorderStatus rs = g.rec->Status();
        if (gameGone) {
            const size_t slash = rs.file.find_last_of(L"\\/");
            PublishFinalToTray(rs, slash == std::wstring::npos ? std::wstring() : rs.file.substr(0, slash));
        }
    }
    ReleaseRing();
    if ((DWORD)g.b->helperPid == GetCurrentProcessId()) InterlockedExchange(&g.b->helperPid, 0);
    g.rec.reset();
    g.src.reset();
    g.ctx4.Reset(); g.ctx.Reset(); g.dev5.Reset(); g.dev.Reset();
    LOGI("helper: exit");
    UnmapViewOfFile(g.b);
    CloseHandle(g.map);
    if (g.wake) CloseHandle(g.wake);
    if (g.quit) CloseHandle(g.quit);
    if (g.readyEvt) CloseHandle(g.readyEvt);
    CloseHandle(g.game);
    if (single) CloseHandle(single);
    return 0;
}
