// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 OR GPL-2.0-or-later
/* live_share_proto -- the shared-memory protocol between the layer (producer, inside the game: src/live_share.cpp) and a
 * live viewer (consumer: the OBS source plugin obs/, tests/live_share_test.cpp; both through obs/live_consumer.cpp).
 * Plain C (also included by C++), x64 only.
 * License: this one header is dual-licensed -- the PolyForm Noncommercial License 1.0.0 (LICENSE.md) OR, at your option,
 * the GNU GPL version 2 or later (obs/COPYING.md) -- so the GPL OBS plugin, which compiles it, stays GPL-clean.
 *
 * One named block, Local\OpenXR-EyeCapture-Live, CREATED BY THE CONSUMER. The layer never creates it: no block (or a
 * stale consumer heartbeat) = no consumer = the layer does nothing beyond one OpenFileMappingW probe per second. The
 * consumer creates it with an explicit security descriptor (full access for SYSTEM, administrators and logged-on users,
 * mandatory label Medium), so a normal game can open the block of an ELEVATED consumer.
 *
 * Producer claim: a producer takes producerPid with InterlockedCompareExchange (0, a dead producer's pid or one whose
 * heartbeat is stale -> its own) BEFORE it sets up, then -- its only writer from now on -- writes slotCount = 0 under the
 * seqlock and starts its heartbeat. One producer per block even when two games probe at the same moment; a producer
 * that later finds another pid there stops and never writes the seqlock part again.
 *
 *   consumer -> producer   consumerHeartbeat (GetTickCount64, ~4x/s), maxFps, reproject (the recorder's re-projection
 *                          per config.ini, or raw eyes), size (full / half), claim, consumerPid
 *   producer -> consumer   seqlock-protected: producer pid + heartbeat, generation (new ring), NT handle values of the
 *                          ring textures and the two fences (valid in the PRODUCER process: the consumer duplicates them
 *                          with OpenProcess(PROCESS_DUP_HANDLE) + DuplicateHandle), ring geometry / format, what the
 *                          ring holds (`reprojected` bits: re-projection, half size, overlays), the latest published slot
 *                          + its ready fence value, counters, game name.
 *                          slotValue[]: per slot, the ready value of the frame that slot holds (0 = being rewritten).
 *
 * Frame hand-off (no CPU wait on either side, no GPU wait on the game's queue):
 *   producer                picks a slot that is neither the latest one nor claimed-and-unfinished, sets slotValue = 0,
 *                           re-checks the claim (Dekker: store, full barrier, load), writes the frame into it -- raw eyes
 *                           at full size: two copies on the GAME queue (game thread); otherwise (re-projected, overlays,
 *                           half size): the pass on the layer's private share device (its worker thread) --, signals
 *                           `ready` = v, sets slotValue = v and publishes (latestSlot, latestValue = v).
 *   consumer                reads (slot, v); skips it until ready has completed v on the CPU; writes claim = (v, slot),
 *                           full barrier, re-reads slotValue[slot] == v (else unclaims and tries next time); GPU
 *                           Wait(ready, v), copies the slot, Signal(free, v). `free` therefore only grows.
 *   producer reuse rule     a slot whose value equals the claimed value is reused only when free >= that value
 *                           (GetCompletedValue on the CPU). Busy: another slot, or the frame is dropped (counted).
 *
 * Version 2: `reproject` (consumer) and `reprojected` (producer, in the former pad0) added; the consumer side grew by
 * 8 bytes, so every producer field moved.
 * Version 3: `size` (consumer, in the former consumerPad) and the `reprojected` bits HALF / OVERLAYS. Same layout as
 * version 2, but a version 2 producer would ignore `size` and a version 2 consumer would not know the new bits.
 */
#ifndef EYECAP_LIVE_SHARE_PROTO_H
#define EYECAP_LIVE_SHARE_PROTO_H
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EYECAP_LIVE_SHM_NAME L"Local\\OpenXR-EyeCapture-Live"

enum {
    EYECAP_LIVE_MAGIC = 0x4556494Cu,     /* 'LIVE' */
    EYECAP_LIVE_VERSION = 3,
    EYECAP_LIVE_MAX_SLOTS = 8,
    EYECAP_LIVE_STALE_MS = 2000,         /* a heartbeat older than this = that side is gone */
    EYECAP_LIVE_DEFAULT_FPS = 60
};

/* EyeCapLive::reproject (what the consumer asks for) */
enum {
    EYECAP_LIVE_REPROJ_CONFIG = 0,       /* same as recording: symmetric_fov / head smoothing per the game's config.ini */
    EYECAP_LIVE_REPROJ_RAW = 1           /* the raw eye images */
};
/* EyeCapLive::size (the ring size the consumer asks for) */
enum {
    EYECAP_LIVE_SIZE_FULL = 0,           /* the eyes at their native (or re-projected) size */
    EYECAP_LIVE_SIZE_HALF = 1            /* half width and height (rendered by the producer's pass, multi-tap minification) */
};
/* EyeCapLive::reprojected (what the ring holds; 0 = the raw eyes as rendered, full size) */
enum {
    EYECAP_LIVE_OUT_SYMMETRIC = 1,       /* both eyes re-projected to one common symmetric frustum (symmetric_fov) */
    EYECAP_LIVE_OUT_SMOOTHED = 2,        /* head-motion smoothing (head_smoothing > 0) */
    EYECAP_LIVE_OUT_HALF = 4,            /* half width and height (size = EYECAP_LIVE_SIZE_HALF) */
    EYECAP_LIVE_OUT_OVERLAYS = 8         /* the game's overlay layers are composited (record_overlays = 1, re-projection mode) */
};

/* claim = (ready value << 4) | slot; 0 = none */
#define EYECAP_LIVE_CLAIM(value, slot) ((int64_t)(((uint64_t)(value) << 4) | ((uint64_t)(slot) & 15u)))
#define EYECAP_LIVE_CLAIM_SLOT(c)      ((uint32_t)((uint64_t)(c) & 15u))
#define EYECAP_LIVE_CLAIM_VALUE(c)     ((uint64_t)(c) >> 4)

typedef struct EyeCapLive {
    uint32_t magic;
    uint32_t version;
    /* ---- consumer side (plain stores; 64-bit aligned fields are atomic on x64) ---- */
    uint32_t consumerPid;
    uint32_t maxFps;                     /* frames per second the consumer wants; 0 = EYECAP_LIVE_DEFAULT_FPS */
    volatile uint64_t consumerHeartbeat; /* GetTickCount64() */
    volatile int64_t claim;              /* EYECAP_LIVE_CLAIM(value, slot) of the frame being read; 0 = none */
    volatile uint32_t reproject;         /* EYECAP_LIVE_REPROJ_*; a change makes the producer build a new ring */
    volatile uint32_t size;              /* EYECAP_LIVE_SIZE_*; a change makes the producer build a new ring */
    /* ---- producer side ---- */
    volatile uint64_t producerHeartbeat; /* GetTickCount64(), every game frame while sharing (outside the seqlock) */
    volatile LONG seq;                   /* seqlock: odd while the producer writes the fields up to `game` */
    uint32_t producerPid;                /* 0 = no producer; claimed by a producer with InterlockedCompareExchange BEFORE it
                                            sets up (slotCount 0 until its ring is published), cleared at its teardown */
    uint32_t generation;                 /* bumped for every new ring (handles below change) */
    uint32_t slotCount;                  /* 0 = no ring */
    uint64_t texHandle[EYECAP_LIVE_MAX_SLOTS];   /* NT handles in the producer process (IDXGIResource1::CreateSharedHandle) */
    uint64_t readyHandle;                /* ID3D11Fence::CreateSharedHandle: signalled after each frame is written */
    uint64_t freeHandle;                 /* signalled by the consumer with the value it consumed */
    uint32_t width, height;              /* ring texture size: left eye at x = 0, right eye at x = leftWidth */
    uint32_t format;                     /* DXGI_FORMAT, typed UNORM / FLOAT (the copy family of the swapchain) */
    uint32_t srgb;                       /* 1 = the bytes are sRGB-encoded (from an *_SRGB swapchain), 0 = linear */
    uint32_t leftWidth, leftHeight, rightWidth, rightHeight;   /* the eyes as they are in the ring (re-projected: output) */
    uint32_t latestSlot;
    uint32_t reprojected;                /* EYECAP_LIVE_OUT_* bits of this ring; 0 = raw eyes, full size */
    uint64_t latestValue;                /* ready fence value of latestSlot; 0 = nothing published yet */
    uint64_t framesShared;               /* frames published in this generation */
    uint64_t framesDropped;              /* no usable slot (consumer still reading / game GPU busy / pass behind) */
    wchar_t  game[64];
    /* ---- per slot (outside the seqlock; see the hand-off above) ---- */
    volatile int64_t slotValue[EYECAP_LIVE_MAX_SLOTS];
} EyeCapLive;

#ifdef __cplusplus
static_assert(offsetof(EyeCapLive, producerPid) % 4 == 0, "producerPid is the target of InterlockedCompareExchange");
#endif

/* Consistent copy of the seqlock-protected producer part (seq .. game). Returns 0 if the producer kept it busy. */
static __inline int EyeCapLiveRead(const EyeCapLive* p, EyeCapLive* out) {
    int tries;
    for (tries = 0; tries < 100; ++tries) {
        const LONG s1 = p->seq;
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(out, (const void*)p, offsetof(EyeCapLive, slotValue));
        MemoryBarrier();
        if (p->seq == s1) return 1;
    }
    return 0;
}

static __inline void EyeCapLiveBeginWrite(EyeCapLive* p) { InterlockedIncrement(&p->seq); MemoryBarrier(); }
static __inline void EyeCapLiveEndWrite(EyeCapLive* p) { MemoryBarrier(); InterlockedIncrement(&p->seq); }

/* A heartbeat is fresh if it is younger than EYECAP_LIVE_STALE_MS (and was ever written). */
static __inline int EyeCapLiveFresh(uint64_t heartbeat, uint64_t now) {
    return heartbeat != 0 && (now < heartbeat || now - heartbeat < (uint64_t)EYECAP_LIVE_STALE_MS);
}

#endif
