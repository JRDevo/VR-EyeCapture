// SPDX-License-Identifier: GPL-2.0-or-later
// live_consumer -- the consumer side of the live share (live_share_proto.h), without libobs: the OBS source plugin
// (plugin.cpp) and tests/live_share_test.cpp both use it, so the test exercises the plugin's exact open / copy code.
//
// Use (all on the thread that owns the device's immediate context -- OBS: the graphics thread):
//   Open(dev)               create / open the block (the producer only looks for it, it never creates it)
//   Heartbeat(fps, reproj, size)  >= 4x per second while the picture is wanted; stop calling = the producer stops within
//                           2 s. reproj = EYECAP_LIVE_REPROJ_CONFIG (the recorder's re-projection per the game's config.ini,
//                           with its overlays) or _RAW; size = EYECAP_LIVE_SIZE_FULL / _HALF. A change of either makes the
//                           producer build a new ring (new generation) about a second later
//   Poll(&info)             producer check: imports a new ring (DuplicateHandle from the game process + OpenSharedResource1
//                           / OpenSharedFence on `dev`), drops a gone one. true = a ring is imported, `info` describes it
//   CopyLatest(dst)         the newest frame, when it is newer than the last one copied AND already complete on the producer's
//                           GPU (checked on the CPU, so the device never waits on the game) and the previous copy has
//                           completed on this device (free >= the last value; until then the old claim stays): claim, GPU
//                           Wait(ready) (already satisfied), CopySubresourceRegion of the whole SBS slot into dst at (0, 0),
//                           Signal(free), Flush.
//   Close()
// Nothing here blocks on the CPU.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include "live_share_proto.h"

struct LiveFrameInfo {
    uint32_t    producerPid = 0, generation = 0;
    uint32_t    width = 0, height = 0;                     // the SBS ring texture
    uint32_t    leftWidth = 0, leftHeight = 0, rightWidth = 0, rightHeight = 0;   // left eye at (0, 0), right at (leftWidth, 0)
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;              // typed (UNORM / FLOAT)
    bool        srgb = true;                               // bytes are sRGB-encoded
    uint32_t    reprojected = 0;                           // EYECAP_LIVE_OUT_* bits (0 = raw eyes, full size)
    std::wstring game;
    uint64_t    framesShared = 0, framesDropped = 0;       // producer counters (this generation)
};

// level: 0 = error, 1 = warning, 2 = info
using LiveLogFn = void (*)(int level, const char* msg);

class LiveConsumer {
public:
    LiveConsumer() = default;
    ~LiveConsumer() { Close(); }
    LiveConsumer(const LiveConsumer&) = delete;
    LiveConsumer& operator=(const LiveConsumer&) = delete;

    bool Open(ID3D11Device* dev, const wchar_t* shmName = EYECAP_LIVE_SHM_NAME, LiveLogFn log = nullptr);
    void Close();
    bool IsOpen() const { return shm_ != nullptr; }
    // true = written now (at most every 200 ms; false also while another consumer process owns the block)
    bool Heartbeat(unsigned maxFps, uint32_t reproject = EYECAP_LIVE_REPROJ_CONFIG, uint32_t size = EYECAP_LIVE_SIZE_FULL);
    bool Poll(LiveFrameInfo* info);
    // 1 = a new frame was copied into dst, 0 = nothing new (or not complete yet / slot rewritten / the previous copy still
    // running on this GPU: try again next frame),
    // -1 = no ring imported. dst: at least info.width x info.height, same copy family as info.format, default usage.
    int CopyLatest(ID3D11Texture2D* dst);
    uint64_t FramesCopied() const { return copied_; }
    uint64_t LastValue() const { return lastValue_; }
    // Test hooks (tests/live_share_test.cpp): hold a claim on the latest slot without signalling `free`, then release it.
    bool TestClaimLatest(uint32_t* slot, uint64_t* value);
    void TestReleaseClaim(uint64_t value);
    const EyeCapLive* Block() const { return shm_; }

private:
    void Log(int level, const char* fmt, ...);
    void Drop(const char* why);
    bool Import(const EyeCapLive& s);

    LiveLogFn log_ = nullptr;
    HANDLE map_ = nullptr;
    EyeCapLive* shm_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11Device1> dev1_;
    Microsoft::WRL::ComPtr<ID3D11Device5> dev5_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> ctx4_;
    // imported ring
    bool imported_ = false;
    uint32_t pid_ = 0, gen_ = 0, slots_ = 0;
    uint32_t failedPid_ = 0, failedGen_ = 0;               // an import that failed is not retried for that generation
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_[EYECAP_LIVE_MAX_SLOTS];
    Microsoft::WRL::ComPtr<ID3D11Fence> ready_, free_;
    LiveFrameInfo info_;
    uint64_t lastValue_ = 0, copied_ = 0;
    ULONGLONG lastBeat_ = 0;
};
