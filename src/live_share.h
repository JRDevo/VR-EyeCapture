// live_share -- the producer side of the live view (OBS source plugin, obs/): hands the game's eye images to a consumer
// process through shared D3D11 textures + fences. Protocol: src/live_share_proto.h. Separate from the recorder: works
// whether or not a recording runs, and the recorder never sees it.
//
// What the viewer gets (the consumer's `reproject` choice, the plugin property "Re-projection"):
//   * "Same as recording" (default): the recorder's flat re-projection with the same config.ini values -- symmetric_fov,
//     head_smoothing, head_follow_deg, roll_lock, output_fov_scale, edge_guard (src/reproject.h, head_smoothing.h) -- and,
//     with record_overlays = 1, the game's overlay layers composited like a recording (overlay.h: the same OverlayPack /
//     OverlayAtlasSet / OverlayCompositor code, into the live share's own atlases). The live share has its OWN HeadSmoother
//     (a recording at the same time keeps its own filter state). With symmetric_fov = 0, head_smoothing = 0 and no
//     overlays this is the raw eyes, like the recording. No dome (projection is ignored: the live view stays flat), no
//     max_width (native size). The in-headset indicator is never in it (appended after the capture, like for recordings).
//   * "Raw eyes": the eye images as the game rendered them (no overlays, no re-projection).
// The consumer's `size` (plugin property "Size"): Full, or Half = the pass renders the exported ring at half width and
// height (2 x 2 taps per output pixel, misc.w); raw eyes at half size go through the same pass with the identity mapping.
// Half saves VRAM and the consumer's copy; the full-size copy on the game's GPU stays the same.
//
// Cost model:
//   * no consumer (no Local\OpenXR-EyeCapture-Live block, or its heartbeat is stale): one OpenFileMappingW probe per
//     second on the game thread, nothing else -- no thread, no device, no log line.
//   * consumer present: a second IFrameSource on the same game device / queue + a private D3D11 "share device" on the
//     game's adapter (CreateEncoderDevice, the recorder's pattern) and its rings, made on a worker thread (never on the
//     game thread). Per shared frame on the game thread: CopyEyes (two copies on the game's GPU, no GPU wait: waitFree = 0;
//     with overlays also their sub-images into the live share's own atlas, in the same submission) + a few stores.
//     Throttled to the consumer's fps (default 60).
//       raw eyes, full size: CopyEyes straight into the exported ring (kLiveSlots SBS textures).
//       the pass (re-projection, overlays and / or half size): CopyEyes into an internal raw ring (kRawSlots, not
//                      exported) + the frame's pose / fov / time / overlay draws into a one-item mailbox; the worker
//                      composites the overlays into that raw slot, runs the filter + the pass on the share device from it
//                      into the exported ring (the output size: symmetric 2 eyeW x eyeH, or the raw layout; halved for
//                      Half) and publishes. A frame the worker has not taken yet is replaced by the next one (counted).
//   * consumer gone for > 2 s: the rings are torn down on the worker thread (texture by texture, ~15 ms apart, so the free never stalls the game's GPU submit), back to probing.
#pragma once
#include <memory>
#include <string>
#include "frame_source.h"
#include "head_smoothing.h"
#include "overlay.h"

struct ID3D12Device;
struct ID3D12CommandQueue;

struct LiveShareParams {
    GfxApi               api = GfxApi::None;   // D3D11 or D3D12 (others: Frame does nothing)
    ID3D11Device*        dev11 = nullptr;      // D3D11 sessions: the game's device
    ID3D12Device*        dev12 = nullptr;      // D3D12 sessions: the game's device + queue (XrGraphicsBindingD3D12KHR)
    ID3D12CommandQueue*  queue12 = nullptr;
    std::wstring         game;                 // exe base name, shown by the consumer
    std::wstring         shmName;              // the block's name (tests: XR_EYE_CAPTURE_TEST_LIVE_SHM)
};

class LiveShare {
public:
    explicit LiveShare(const LiveShareParams& p);
    ~LiveShare();   // stops sharing (waits for the worker's teardown, at most a few seconds)
    // Game thread, inside xrEndFrame (g_lock held), once per frame of an armed session. L / R = the recorder's views
    // (after swap_eyes, rectangles clipped to the images, with their pose / fov). stage / srgb: the ring format and whether
    // the bytes are sRGB-encoded (the recorder's decision, gamma override applied). ts100ns: frame time (100 ns units,
    // displayTime / 100), for max fps and the smoothing filter. head: the current config.ini re-projection keys (g_cfg.head;
    // a change while sharing re-projected builds a new ring). overlaysOn: record_overlays (a change while sharing in the
    // re-projection mode builds a new ring); ov: this frame's overlay layers (CollectOverlays; nullptr = none) -- only
    // read while WantsOverlays().
    void Frame(const EyeView& L, const EyeView& R, DXGI_FORMAT stage, bool srgb, int64_t ts100ns,
               const HeadSmoothingSettings& head, bool overlaysOn = false, const OverlayFrame* ov = nullptr);
    // Game thread: the live view composites overlay layers right now (the caller collects them for Frame only then).
    bool WantsOverlays() const;
    // Game thread: a consumer is receiving frames (sharing, a frame published within the last second) -- the in-headset
    // indicator's live state (indicator_live).
    bool Active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
