// x86_layer_stubs -- the parts of eye_capture_core the 32-bit layer eye_capture32.dll does not have (docs/X86_DESIGN.md).
// The 32-bit layer is src/layer.cpp compiled unchanged against a smaller core: its Recorder is src/recorder_remote.cpp
// (the eyes go to the 64-bit EyeCaptureRecorder.exe), and these stand in for what must never run inside a 2 GB process or is
// not implemented for 32-bit games:
//   * LiveShare (the OBS live view, live_share.cpp: x64 only, a second D3D11 device and its rings) -- a no-op that logs once;
//   * CreateD3D12FrameSource (source_d3d12.cpp, d3d12.dll) -- 32-bit D3D12 sessions are not captured (logged).
#include <windows.h>
#include <atomic>
#include "frame_source.h"
#include "live_share.h"
#include "log.h"

static_assert(sizeof(void*) == 4, "x86_layer_stubs.cpp belongs to the 32-bit layer only");

struct LiveShare::Impl {};

LiveShare::LiveShare(const LiveShareParams&) : impl_(std::make_unique<Impl>()) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        LOGI("live share: the OBS live view is not available for 32-bit games (live_share is ignored)");
}
LiveShare::~LiveShare() = default;
void LiveShare::Frame(const EyeView&, const EyeView&, DXGI_FORMAT, bool, int64_t, const HeadSmoothingSettings&, bool, const OverlayFrame*) {}
bool LiveShare::WantsOverlays() const { return false; }
bool LiveShare::Active() const { return false; }

std::unique_ptr<IFrameSource> CreateD3D12FrameSource(ID3D12Device*, ID3D12CommandQueue*) {
    LOGW("D3D12 session in a 32-bit game: not supported (32-bit games are recorded through Direct3D 11 only) -- capture disabled, "
         "the game runs normally");
    return nullptr;
}
