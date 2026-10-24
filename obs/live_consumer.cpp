// SPDX-License-Identifier: GPL-2.0-or-later
// live_consumer -- see live_consumer.h; protocol in live_share_proto.h.
#include "live_consumer.h"
#include <sddl.h>
#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "advapi32.lib")   // ConvertStringSecurityDescriptorToSecurityDescriptorW

using Microsoft::WRL::ComPtr;

void LiveConsumer::Log(int level, const char* fmt, ...) {
    if (!log_) return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    log_(level, b);
}

bool LiveConsumer::Open(ID3D11Device* dev, const wchar_t* shmName, LiveLogFn log) {
    Close();
    log_ = log;
    if (!dev) return false;
    dev_ = dev;
    dev_->GetImmediateContext(&ctx_);
    if (FAILED(dev_.As(&dev1_)) || FAILED(dev_.As(&dev5_)) || FAILED(ctx_.As(&ctx4_))) {
        Log(0, "the D3D11 device has no ID3D11Device5 / ID3D11DeviceContext4 (shared fences need Windows 10 1703+)");
        Close();
        return false;
    }
    // Explicit security: the default one of an ELEVATED OBS (DACL: administrators + SYSTEM, integrity label High) would make
    // the normal game's OpenFileMappingW fail with access denied. SYSTEM, administrators and every logged-on user get full
    // access, the mandatory label is Medium (no write up from below Medium; a normal game is Medium). Only the object this
    // side creates needs it: the textures / fences are duplicated from the game process, never opened by name.
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{(DWORD)sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)S:(ML;;NW;;;ME)",
                                                             SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;
    else
        Log(1, "ConvertStringSecurityDescriptorToSecurityDescriptor failed (error %lu) -- default security (an elevated OBS "
            "then hides the block from a normal game)", GetLastError());
    map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(EyeCapLive), shmName);
    const DWORD mapErr = GetLastError();
    if (sd) LocalFree(sd);
    if (!map_) { Log(0, "CreateFileMapping failed (error %lu)", mapErr); Close(); return false; }
    const bool created = mapErr != ERROR_ALREADY_EXISTS;
    shm_ = (EyeCapLive*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EyeCapLive));
    if (!shm_) { Log(0, "MapViewOfFile failed (error %lu)", GetLastError()); Close(); return false; }
    if (created || shm_->magic != EYECAP_LIVE_MAGIC) {
        shm_->version = EYECAP_LIVE_VERSION;   // zero-filled by the OS on creation
        MemoryBarrier();
        shm_->magic = EYECAP_LIVE_MAGIC;
    } else if (shm_->version != EYECAP_LIVE_VERSION) {
        Log(1, "the live block has version %u, this plugin speaks %u -- the producer will ignore it", shm_->version,
            (unsigned)EYECAP_LIVE_VERSION);
    }
    return true;
}

void LiveConsumer::Close() {
    Drop(nullptr);
    if (shm_) {
        if (shm_->consumerPid == GetCurrentProcessId()) {
            InterlockedExchange64(&shm_->claim, 0);
            shm_->consumerHeartbeat = 0;   // the producer stops at its next frame instead of after 2 s
        }
        UnmapViewOfFile(shm_);
    }
    if (map_) CloseHandle(map_);
    shm_ = nullptr;
    map_ = nullptr;
    ctx4_.Reset(); ctx_.Reset(); dev5_.Reset(); dev1_.Reset(); dev_.Reset();
}

bool LiveConsumer::Heartbeat(unsigned maxFps, uint32_t reproject, uint32_t size) {
    if (!shm_) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - lastBeat_ < 200) return false;   // ~5x per second is plenty (stale after 2 s)
    // another consumer process with a fresh heartbeat owns the block: leave it alone
    const DWORD other = shm_->consumerPid;
    if (other && other != GetCurrentProcessId() && EyeCapLiveFresh(shm_->consumerHeartbeat, now)) {
        if (!lastBeat_) Log(1, "another live viewer (pid %lu) is using the block -- this one stays idle", (unsigned long)other);
        lastBeat_ = now;
        return false;
    }
    lastBeat_ = now;
    shm_->consumerPid = GetCurrentProcessId();
    shm_->maxFps = maxFps ? maxFps : (unsigned)EYECAP_LIVE_DEFAULT_FPS;
    shm_->reproject = reproject == EYECAP_LIVE_REPROJ_RAW ? (uint32_t)EYECAP_LIVE_REPROJ_RAW : (uint32_t)EYECAP_LIVE_REPROJ_CONFIG;
    shm_->size = size == EYECAP_LIVE_SIZE_HALF ? (uint32_t)EYECAP_LIVE_SIZE_HALF : (uint32_t)EYECAP_LIVE_SIZE_FULL;
    shm_->consumerHeartbeat = now;
    return true;
}

void LiveConsumer::Drop(const char* why) {
    if (imported_ && why) Log(2, "producer pid %u generation %u released (%s) after %llu frames copied", pid_, gen_, why,
                              (unsigned long long)copied_);
    if (shm_ && imported_) InterlockedExchange64(&shm_->claim, 0);
    for (auto& t : tex_) t.Reset();
    ready_.Reset();
    free_.Reset();
    imported_ = false;
    pid_ = gen_ = slots_ = 0;
    info_ = LiveFrameInfo();
    lastValue_ = 0;
}

bool LiveConsumer::Import(const EyeCapLive& s) {
    HANDLE proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, s.producerPid);
    if (!proc) {
        Log(0, "OpenProcess(PROCESS_DUP_HANDLE) on the game (pid %u) failed (error %lu) -- is the game elevated and this "
            "process not?", s.producerPid, GetLastError());
        return false;
    }
    auto dup = [&](uint64_t v, HANDLE* out) -> bool {
        *out = nullptr;
        return v && DuplicateHandle(proc, (HANDLE)(uintptr_t)v, GetCurrentProcess(), out, 0, FALSE, DUPLICATE_SAME_ACCESS);
    };
    bool ok = true;
    for (uint32_t i = 0; ok && i < s.slotCount; ++i) {
        HANDLE h = nullptr;
        if (!dup(s.texHandle[i], &h)) { Log(0, "DuplicateHandle(ring %u) failed (error %lu)", i, GetLastError()); ok = false; break; }
        const HRESULT hr = dev1_->OpenSharedResource1(h, IID_PPV_ARGS(&tex_[i]));
        CloseHandle(h);
        if (FAILED(hr)) { Log(0, "OpenSharedResource1(ring %u) failed hr=0x%08lx", i, (unsigned long)hr); ok = false; break; }
        D3D11_TEXTURE2D_DESC d{};
        tex_[i]->GetDesc(&d);
        if (d.Width != s.width || d.Height != s.height || d.Format != (DXGI_FORMAT)s.format) {
            Log(0, "ring %u is %ux%u format %u, the block says %ux%u format %u", i, d.Width, d.Height, (unsigned)d.Format,
                s.width, s.height, s.format);
            ok = false;
        }
    }
    HANDLE fh[2] = {};
    ComPtr<ID3D11Fence>* fences[2] = {&ready_, &free_};
    const uint64_t fv[2] = {s.readyHandle, s.freeHandle};
    for (int i = 0; ok && i < 2; ++i) {
        if (!dup(fv[i], &fh[i])) { Log(0, "DuplicateHandle(fence %d) failed (error %lu)", i, GetLastError()); ok = false; break; }
        const HRESULT hr = dev5_->OpenSharedFence(fh[i], IID_PPV_ARGS(&*fences[i]));
        CloseHandle(fh[i]);
        if (FAILED(hr)) { Log(0, "OpenSharedFence(%s) failed hr=0x%08lx", i ? "free" : "ready", (unsigned long)hr); ok = false; }
    }
    CloseHandle(proc);
    if (!ok) {
        for (auto& t : tex_) t.Reset();
        ready_.Reset();
        free_.Reset();
        return false;
    }
    imported_ = true;
    pid_ = s.producerPid;
    gen_ = s.generation;
    slots_ = s.slotCount;
    lastValue_ = 0;
    copied_ = 0;
    info_.producerPid = s.producerPid;
    info_.generation = s.generation;
    info_.width = s.width; info_.height = s.height;
    info_.leftWidth = s.leftWidth; info_.leftHeight = s.leftHeight;
    info_.rightWidth = s.rightWidth; info_.rightHeight = s.rightHeight;
    info_.format = (DXGI_FORMAT)s.format;
    info_.srgb = s.srgb != 0;
    info_.reprojected = s.reprojected;
    wchar_t g[65];
    memcpy(g, s.game, sizeof(s.game));
    g[64] = 0;
    info_.game = g;
    char gn[130];
    WideCharToMultiByte(CP_UTF8, 0, g, -1, gn, (int)sizeof(gn), nullptr, nullptr);
    const uint32_t rp = s.reprojected & (EYECAP_LIVE_OUT_SYMMETRIC | EYECAP_LIVE_OUT_SMOOTHED);
    const char* what = !rp ? "raw eyes"
                     : rp == EYECAP_LIVE_OUT_SYMMETRIC ? "re-projected: symmetric fov"
                     : rp == EYECAP_LIVE_OUT_SMOOTHED ? "re-projected: head smoothing"
                                                      : "re-projected: symmetric fov + head smoothing";
    Log(2, "producer '%s' pid %u generation %u imported: %u slots %ux%u DXGI format %u (%s), eyes %ux%u + %ux%u, %s%s%s", gn,
        s.producerPid, s.generation, s.slotCount, s.width, s.height, s.format, s.srgb ? "sRGB-encoded" : "linear",
        s.leftWidth, s.leftHeight, s.rightWidth, s.rightHeight, what,
        (s.reprojected & EYECAP_LIVE_OUT_OVERLAYS) ? " + overlays" : "", (s.reprojected & EYECAP_LIVE_OUT_HALF) ? ", half size" : "");
    return true;
}

bool LiveConsumer::Poll(LiveFrameInfo* info) {
    if (!shm_) return false;
    EyeCapLive s;
    if (!EyeCapLiveRead(shm_, &s)) return imported_;   // the producer is writing: keep the current state
    const ULONGLONG now = GetTickCount64();
    const bool live = s.producerPid && s.slotCount && s.slotCount <= EYECAP_LIVE_MAX_SLOTS && s.width && s.height &&
                      EyeCapLiveFresh(shm_->producerHeartbeat, now);
    if (!live) {
        // pid 0 = torn down; a pid with slotCount 0 = a producer that just claimed the block and is still setting up
        if (imported_) Drop(s.producerPid && s.slotCount ? "producer heartbeat stale" : "producer stopped sharing");
        return false;
    }
    if (imported_ && (s.generation != gen_ || s.producerPid != pid_)) Drop("new ring");
    if (!imported_) {
        if (s.producerPid == failedPid_ && s.generation == failedGen_) return false;
        if (!Import(s)) {
            failedPid_ = s.producerPid;
            failedGen_ = s.generation;
            return false;
        }
    }
    info_.framesShared = s.framesShared;
    info_.framesDropped = s.framesDropped;
    if (info) *info = info_;
    return true;
}

int LiveConsumer::CopyLatest(ID3D11Texture2D* dst) {
    if (!imported_ || !shm_ || !dst) return -1;
    if (shm_->consumerPid != GetCurrentProcessId()) return 0;   // another viewer process owns the claim
    EyeCapLive s;
    if (!EyeCapLiveRead(shm_, &s)) return 0;
    if (s.generation != gen_ || s.producerPid != pid_) return 0;   // Poll re-imports
    const uint32_t slot = s.latestSlot;
    const uint64_t v = s.latestValue;
    if (!v || v <= lastValue_ || slot >= slots_) return 0;
    // The previous copy must have completed on THIS GPU first: the producer's reuse rule protects only the claimed slot, so a
    // new claim while Signal(free, lastValue_) is still queued would let it rewrite the slot being copied (tearing). Keep
    // the old claim until then. Cannot stall for good: a removed device reports UINT64_MAX, and Drop / Import reset
    // lastValue_ (a new ring or a gone producer never waits on the old fence).
    if (lastValue_ && free_->GetCompletedValue() < lastValue_) return 0;
    if (ready_->GetCompletedValue() < v) return 0;               // not finished on the producer's GPU yet: never wait for it
    InterlockedExchange64(&shm_->claim, EYECAP_LIVE_CLAIM(v, slot));   // full barrier
    if ((uint64_t)shm_->slotValue[slot] != v) {                  // the producer is rewriting that slot: next time
        InterlockedExchange64(&shm_->claim, 0);
        return 0;
    }
    ctx4_->Wait(ready_.Get(), v);   // already complete (checked above): orders the copy after the producer's write all the same
    const D3D11_BOX box{0, 0, 0, info_.width, info_.height, 1};
    ctx_->CopySubresourceRegion(dst, 0, 0, 0, 0, tex_[slot].Get(), 0, &box);
    ctx4_->Signal(free_.Get(), v);
    ctx_->Flush();                  // the producer reads `free` on the CPU: get the signal executed
    lastValue_ = v;
    ++copied_;
    return 1;
}

bool LiveConsumer::TestClaimLatest(uint32_t* slot, uint64_t* value) {
    if (!imported_) return false;
    EyeCapLive s;
    // only a frame not consumed yet (free < value): a claim on a consumed one would not hold the producer off
    if (!EyeCapLiveRead(shm_, &s) || !s.latestValue || s.latestValue <= lastValue_ || s.latestSlot >= slots_) return false;
    if (lastValue_ && free_->GetCompletedValue() < lastValue_) return false;   // CopyLatest's rule: the last copy first
    InterlockedExchange64(&shm_->claim, EYECAP_LIVE_CLAIM(s.latestValue, s.latestSlot));
    if ((uint64_t)shm_->slotValue[s.latestSlot] != s.latestValue) { InterlockedExchange64(&shm_->claim, 0); return false; }
    *slot = s.latestSlot;
    *value = s.latestValue;
    return true;
}

void LiveConsumer::TestReleaseClaim(uint64_t value) {
    if (!imported_) return;
    ctx4_->Signal(free_.Get(), value);
    ctx_->Flush();
    if (value > lastValue_) lastValue_ = value;
}
