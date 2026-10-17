// live_share_test -- a live-share consumer exactly like the OBS plugin (same obs/live_consumer.cpp, same protocol header),
// run against a game that is driven by tests/layer_e2e.exe through the real loader, our layer and the mock runtime
// (tests/run_live.ps1 starts both). No OBS, no headset.
//
//   live_share_test [--shm <name>] [--timeout <s>] [--eye WxH] [--raw] [--half] [--expect raw|sym|smooth|overlays]
//                   [--flags <n>] [--switch-raw] [--abandon] [--idle <s>]
//
// --raw: ask for the raw eyes (the plugin's "Re-projection: Raw eyes"); default = the re-projection per config.ini.
// --half: ask for half size (the plugin's "Size: Half"): the ring must be half the expected eye sizes ((n + 1) / 2), the
//   `reprojected` bits include HALF, and the pixel checks run on the half-size eyes (pattern within 2 code values: the
//   2 x 2 taps of the pass average texels of the same pattern step).
// --expect (what the ring must hold; default raw):
//   raw     raw eyes: the eye sizes are the mock's (--eye), the copied pixels of BOTH eyes are the test pattern
//           (tests/test_pattern.h: grey steps, primaries, eye-id colour, ramp) bit-exactly.
//   sym     symmetric fov only, against the plain mock (identity pose, +-0.8 rad fov, i.e. already symmetric): eyes =
//           ComputeSymmetricFov of that (= the mock's size), the pattern within 2 code values (bilinear identity mapping).
//   smooth  symmetric fov + head smoothing against layer_e2e --jitter (tests/test_motion.h: asymmetric fovs, head jitter
//           + a 30 degree turn; world-locked grid + a white marker at -Z): eyes = ComputeSymmetricFov(kFovLeft, kFovRight,
//           --eye), the marker at the same x / y in both halves within 1.5 px (parallel symmetric cameras; the raw
//           asymmetric eyes are ~250 px apart), no black corner in either eye (edge guard).
//   overlays  "same as recording" with record_overlays = 1 against layer_e2e --overlays (plain symmetric mock, eyes =
//           --eye): the overlay layers are in the live frame where tests/run_e2e.ps1 finds them in a recording (CheckOverlays:
//           opaque quad, opaque cylinder, back-facing quad not drawn) and the in-headset indicator is not; the generation
//           with the OVERLAYS bit is waited for (the first ring has none until the game's first overlay layer).
// --flags <n>: the expected `reprojected` bits (default: from --expect / --half: raw 0, sym 1, smooth 3, overlays 1 | 8,
//   + 4 for --half).
// Every mode: the block's `reprojected` bits match; frames arrive at the requested rate (60 fps, then 30 fps: the
// producer's throttle); while a claim on the latest slot is held without signalling `free`, the producer keeps publishing
// and never re-publishes that slot.
// --switch-raw (after the checks): switch to raw eyes; the producer must publish a NEW generation with the raw eye sizes
// and `reprojected` = 0 within 6 s, and frames must flow from it.
// Exit: Close() (heartbeat cleared: the producer stops at once), or --abandon: the process ends without Close (the
// producer must stop on the 2 s heartbeat timeout). --idle <s>: create the block but never heartbeat for s seconds (the
// producer must not start). Prints LIVE_OK / LIVE_IDLE_OK on success, LIVE FAIL: ... otherwise.
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "head_smoothing.h"
#include "live_consumer.h"
#include "test_motion.h"
#include "test_pattern.h"

using Microsoft::WRL::ComPtr;

namespace {
uint32_t g_mode = EYECAP_LIVE_REPROJ_CONFIG;
uint32_t g_size = EYECAP_LIVE_SIZE_FULL;

void LogFn(int level, const char* msg) {
    printf("  consumer %s: %s\n", level == 0 ? "ERROR" : level == 1 ? "warn" : "info", msg);
    fflush(stdout);
}

int Fail(const char* what) {
    printf("LIVE FAIL: %s\n", what);
    fflush(stdout);
    return 1;
}

// BGRA bytes of pixel (x, y) of a mapped B8G8R8A8 texture -> r, g, b
void Px(const D3D11_MAPPED_SUBRESOURCE& m, int x, int y, int rgb[3]) {
    const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch + (size_t)x * 4;
    rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
}

// One eye of the copied SBS frame against the pattern (tol 0: exact bytes, the copies never change a value).
int CheckEye(const D3D11_MAPPED_SUBRESOURCE& m, int eye, int ox, int w, int h, int tol) {
    int bad = 0, worst = 0;
    auto expect = [&](int x, int y, int r, int g, int b, const char* what) {
        int v[3];
        Px(m, ox + x, y, v);
        const int d = std::max(std::abs(v[0] - r), std::max(std::abs(v[1] - g), std::abs(v[2] - b)));
        if (d > worst) worst = d;
        if (d > tol) {
            if (++bad <= 8)
                printf("  eye %d %s at (%d, %d): got (%d, %d, %d), want (%d, %d, %d)\n", eye, what, x, y, v[0], v[1], v[2], r, g, b);
        }
    };
    const int hA = h / 4, hB = h / 2, hC = 5 * h / 8, hD = 3 * h / 4;
    for (int i = 0; i < 9; ++i) expect((2 * i + 1) * w / 18, hA / 2, kPatGrey[i], kPatGrey[i], kPatGrey[i], "grey step");
    const int prim[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int i = 0; i < 4; ++i) expect((2 * i + 1) * w / 8, (hA + hB) / 2, prim[i][0], prim[i][1], prim[i][2], "primary");
    for (int x = 8; x < w; x += w / 7) {
        if (eye == 0) expect(x, (hB + hC) / 2, 200, 40, 40, "eye id (left)");
        else expect(x, (hB + hC) / 2, 40, 40, 200, "eye id (right)");
    }
    for (int i = 0; i < 64; i += 7) expect((2 * i + 1) * w / 128, (hC + hD) / 2, i * 4, i * 4, i * 4, "ramp");
    printf("  eye %d (x %d..%d): %s (worst difference %d, allowed %d)\n", eye, ox, ox + w,
           bad ? "MISMATCH" : "pattern ok (grey steps, primaries, eye id, ramp)", worst, tol);
    return bad;
}

// layer_e2e --jitter scene in one eye: centroid of the white marker (> 200) and the darkest of the four corners.
struct SceneEye { bool marker = false; double mx = 0, my = 0; int n = 0; int darkestCorner = 255; };
SceneEye Scene(const D3D11_MAPPED_SUBRESOURCE& m, int ox, int w, int h) {
    SceneEye s;
    double sx = 0, sy = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int v[3];
            Px(m, ox + x, y, v);
            if (v[1] > 200) { sx += x; sy += y; ++s.n; }
        }
    }
    if (s.n >= 4) { s.marker = true; s.mx = sx / s.n; s.my = sy / s.n; }
    const int cx[4] = {1, w - 2, 1, w - 2}, cy[4] = {1, 1, h - 2, h - 2};
    for (int i = 0; i < 4; ++i) {
        int v[3];
        Px(m, ox + cx[i], cy[i], v);
        s.darkestCorner = std::min(s.darkestCorner, std::max(v[0], std::max(v[1], v[2])));
    }
    return s;
}

// A producer generation other than notGeneration; wantFlags != ~0u: one whose `reprojected` bits are these (earlier
// generations are reported and skipped: with overlays the first ring has none until the game's first overlay layer
// restarts it with them).
bool WaitProducer(LiveConsumer& c, double timeout, LiveFrameInfo* info, uint32_t notGeneration, uint32_t wantFlags = ~0u) {
    const ULONGLONG t0 = GetTickCount64();
    uint32_t seen = 0;
    while (GetTickCount64() - t0 < (ULONGLONG)(timeout * 1000)) {
        c.Heartbeat(60, g_mode, g_size);
        if (c.Poll(info) && info->generation != notGeneration) {
            if (wantFlags == ~0u || info->reprojected == wantFlags) return true;
            if (info->generation != seen) {
                seen = info->generation;
                printf("  generation %u has reprojected %u (want %u): waiting for the next one\n", info->generation,
                       info->reprojected, wantFlags);
            }
        }
        Sleep(5);
    }
    return false;
}

// --expect overlays: layer_e2e --overlays (tests/run_e2e.ps1 has the same points): the mock's eyes at (-+0.032, 1.6, 0),
// identity orientation, fov +-0.8 rad; a projection-space point -> pixel of eye `eye` in an eyeW x eyeH eye (x in the SBS
// frame).
void OvPx(double x, double y, double z, int eye, int ew, int eh, int& px, int& py) {
    const double T = std::tan(0.8), ex = eye ? 0.032 : -0.032;
    const double u = ((x - ex) / -z + T) / (2 * T), v = (T - (y - 1.6) / -z) / (2 * T);
    px = (int)std::lround(u * ew) + eye * ew;
    py = (int)std::lround(v * eh);
}

// Average colour of a (2 dx + 1) x (2 dy + 1) box; worst channel difference to want.
int BoxDiff(const D3D11_MAPPED_SUBRESOURCE& m, int cx, int cy, int dx, int dy, const int want[3], int got[3]) {
    double s[3] = {0, 0, 0};
    int n = 0;
    for (int y = cy - dy; y <= cy + dy; ++y)
        for (int x = cx - dx; x <= cx + dx; ++x) {
            int v[3];
            Px(m, x, y, v);
            for (int k = 0; k < 3; ++k) s[k] += v[k];
            ++n;
        }
    int worst = 0;
    for (int k = 0; k < 3; ++k) {
        got[k] = (int)std::lround(s[k] / n);
        worst = std::max(worst, std::abs(got[k] - want[k]));
    }
    return worst;
}

// The overlay layers in the live frame (both eyes): A opaque green quad, C opaque orange cylinder, D back-facing quad
// (never drawn: the pattern's eye-id band), and the region where the in-headset indicator is seen (never in the live
// view: the eye-id band). Returns the number of failed checks.
int CheckOverlays(const D3D11_MAPPED_SUBRESOURCE& m, int ew, int eh) {
    const double sx = ew / 1920.0, sy = eh / 1080.0;   // box sizes scale with the eye (Half)
    int bad = 0;
    for (int eye = 0; eye < 2; ++eye) {
        const int band[3] = {eye ? 40 : 200, 40, eye ? 200 : 40};
        const int green[3] = {0, 255, 0}, orange[3] = {255, 128, 0};
        struct P { const char* what; double x, y, z; const int* want; int dx, dy; } pts[] = {
            {"A quad (opaque green)", 0.0, 1.45, -2.0, green, 20, 15},
            {"C cylinder (opaque orange)", 0.5, 2.5, -1.5, orange, 10, 6},
            {"D back-facing quad (not drawn: band)", 0.7, 1.41, -1.5, band, 10, 6},
        };
        for (const P& p : pts) {
            int px, py, got[3];
            OvPx(p.x, p.y, p.z, eye, ew, eh, px, py);
            const int d = BoxDiff(m, px, py, std::max(1, (int)(p.dx * sx)), std::max(1, (int)(p.dy * sy)), p.want, got);
            printf("  eye %d %s at (%d, %d): (%d, %d, %d) want (%d, %d, %d) -> %s\n", eye, p.what, px, py, got[0], got[1], got[2],
                   p.want[0], p.want[1], p.want[2], d <= 12 ? "ok" : "FAIL");
            if (d > 12) ++bad;
        }
        // indicator region (view space (0.22, -0.16, -0.8) at fov +-0.8: x 1176..1256, y 620..670 of a 1920x1080 eye)
        int worst = 0;
        for (int y = (int)(620 * sy); y < (int)(670 * sy); ++y)
            for (int x = (int)(1176 * sx); x < (int)(1256 * sx); ++x) {
                int v[3];
                Px(m, x + eye * ew, y, v);
                for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(v[k] - band[k]));
            }
        printf("  eye %d indicator region: worst difference to the band %d -> %s\n", eye, worst,
               worst <= 12 ? "ok: the indicator is not in the live view" : "FAIL");
        if (worst > 12) ++bad;
    }
    return bad;
}
} // namespace

int main(int argc, char** argv) {
    std::wstring shm = EYECAP_LIVE_SHM_NAME;
    double timeout = 15, idle = 0;
    unsigned eyeW = 1920, eyeH = 1080;
    bool abandon = false, switchRaw = false;
    std::string expect = "raw";
    long long flagsArg = -1;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (k == "--shm") { std::string v = next(); shm.assign(v.begin(), v.end()); }
        else if (k == "--timeout") timeout = atof(next().c_str());
        else if (k == "--eye") { std::string v = next(); sscanf_s(v.c_str(), "%ux%u", &eyeW, &eyeH); }
        else if (k == "--raw") g_mode = EYECAP_LIVE_REPROJ_RAW;
        else if (k == "--half") g_size = EYECAP_LIVE_SIZE_HALF;
        else if (k == "--flags") flagsArg = atoll(next().c_str());
        else if (k == "--expect") expect = next();
        else if (k == "--switch-raw") switchRaw = true;
        else if (k == "--abandon") abandon = true;
        else if (k == "--idle") idle = atof(next().c_str());
        else { printf("unknown argument %s\n", k.c_str()); return 2; }
    }
    if (expect != "raw" && expect != "sym" && expect != "smooth" && expect != "overlays") {
        printf("--expect raw|sym|smooth|overlays\n");
        return 2;
    }
    const bool half = g_size == EYECAP_LIVE_SIZE_HALF;
    printf("live_share_test: block %ls, eye %ux%u, asks for %s at %s size, expects %s%s%s\n", shm.c_str(), eyeW, eyeH,
           g_mode == EYECAP_LIVE_REPROJ_RAW ? "raw eyes" : "the re-projection (config.ini)", half ? "half" : "full",
           expect.c_str(), switchRaw ? ", then switches to raw" : "", abandon ? ", abandon at the end" : "");
    fflush(stdout);

    // expected ring geometry and `reprojected` bits
    uint32_t wantFlags = 0, wantW = eyeW, wantH = eyeH;
    int tol = 0;
    if (expect == "sym") {
        const float f[4] = {-0.8f, 0.8f, 0.8f, -0.8f};
        const SymmetricFov s = ComputeSymmetricFov(f, f, eyeW, eyeH, eyeW, eyeH);
        wantFlags = EYECAP_LIVE_OUT_SYMMETRIC; wantW = s.eyeW; wantH = s.eyeH; tol = 2;
    } else if (expect == "smooth") {
        const SymmetricFov s = ComputeSymmetricFov(test_motion::kFovLeft, test_motion::kFovRight, eyeW, eyeH, eyeW, eyeH);
        wantFlags = EYECAP_LIVE_OUT_SYMMETRIC | EYECAP_LIVE_OUT_SMOOTHED; wantW = s.eyeW; wantH = s.eyeH;
    } else if (expect == "overlays") {   // symmetric fov of the symmetric mock = its own size; the identity mapping
        wantFlags = EYECAP_LIVE_OUT_SYMMETRIC | EYECAP_LIVE_OUT_OVERLAYS; tol = 2;
    }
    if (half) {
        wantFlags |= EYECAP_LIVE_OUT_HALF;
        wantW = wantW > 1 ? (wantW + 1) / 2 : 1;
        wantH = wantH > 1 ? (wantH + 1) / 2 : 1;
        tol = std::max(tol, 2);
    }
    if (flagsArg >= 0) wantFlags = (uint32_t)flagsArg;

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return Fail("D3D11CreateDevice");
    LiveConsumer c;
    if (!c.Open(dev.Get(), shm.c_str(), LogFn)) return Fail("Open");

    if (idle > 0) {   // the block exists, but nobody heartbeats: the producer must stay idle
        const ULONGLONG end = GetTickCount64() + (ULONGLONG)(idle * 1000);
        while (GetTickCount64() < end) {
            if (c.Block()->producerPid) return Fail("a producer started although the consumer never heartbeat");
            Sleep(20);
        }
        printf("LIVE_IDLE_OK block open %.1f s without heartbeat, no producer\n", idle);
        return 0;
    }

    // ---- wait for a producer ----
    LiveFrameInfo info;
    const ULONGLONG t0 = GetTickCount64();
    if (!WaitProducer(c, timeout, &info, 0, wantFlags))
        return Fail("no producer (with the expected reprojected bits) within the timeout");
    printf("  producer after %.2f s: '%ls' pid %u generation %u, %ux%u format %d srgb %d, eyes %ux%u + %ux%u, reprojected %u\n",
           (GetTickCount64() - t0) / 1000.0, info.game.c_str(), info.producerPid, info.generation, info.width, info.height,
           (int)info.format, info.srgb ? 1 : 0, info.leftWidth, info.leftHeight, info.rightWidth, info.rightHeight, info.reprojected);
    if (info.reprojected != wantFlags) {
        printf("  want reprojected %u\n", wantFlags);
        return Fail("the ring holds something else than expected (reprojected bits)");
    }
    if (info.leftWidth != wantW || info.rightWidth != wantW || info.leftHeight != wantH || info.rightHeight != wantH ||
        info.width != 2 * wantW || info.height != wantH) {
        printf("  want eyes %ux%u, SBS %ux%u\n", wantW, wantH, 2 * wantW, wantH);
        return Fail(expect == "raw" ? "eye sizes differ from the mock's" : "eye sizes differ from the symmetric output size");
    }
    if (info.format != DXGI_FORMAT_B8G8R8A8_UNORM || !info.srgb) return Fail("format: want B8G8R8A8_UNORM, sRGB-encoded");

    D3D11_TEXTURE2D_DESC d{};
    d.Width = info.width; d.Height = info.height; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;   // like OBS's gs textures: typeless resource, copy-compatible with the ring
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> dst, staging;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &dst))) return Fail("dst texture");
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return Fail("staging texture");

    // ---- frame rate at 60, then 30 fps (the producer throttles to the requested rate) ----
    auto measure = [&](unsigned fps, double sec, double* rate) -> bool {
        c.Heartbeat(fps, g_mode, g_size);
        // let the producer see the new rate and settle
        ULONGLONG end = GetTickCount64() + 300;
        while (GetTickCount64() < end) { c.Heartbeat(fps, g_mode, g_size); c.Poll(nullptr); c.CopyLatest(dst.Get()); Sleep(2); }
        LiveFrameInfo a;
        if (!c.Poll(&a)) return false;
        const uint64_t copied0 = c.FramesCopied();
        const ULONGLONG s = GetTickCount64();
        end = s + (ULONGLONG)(sec * 1000);
        while (GetTickCount64() < end) {
            c.Heartbeat(fps, g_mode, g_size);
            if (!c.Poll(nullptr)) return false;
            c.CopyLatest(dst.Get());
            Sleep(2);
        }
        LiveFrameInfo b;
        if (!c.Poll(&b)) return false;
        const double dt = (GetTickCount64() - s) / 1000.0;
        *rate = (b.framesShared - a.framesShared) / dt;
        printf("  max fps %u: producer published %.1f frames/s, consumer copied %llu frames in %.2f s, producer dropped %llu\n",
               fps, *rate, (unsigned long long)(c.FramesCopied() - copied0), dt, (unsigned long long)b.framesDropped);
        return (c.FramesCopied() - copied0) >= (uint64_t)(fps * sec * 0.6);
    };
    double r60 = 0, r30 = 0;
    if (!measure(60, 2.0, &r60)) return Fail("too few frames copied at 60 fps (or the producer went away)");
    if (r60 < 50 || r60 > 70) return Fail("producer rate at max fps 60 is not ~60");
    if (!measure(30, 2.0, &r30)) return Fail("too few frames copied at 30 fps (or the producer went away)");
    if (r30 < 24 || r30 > 36) return Fail("producer rate at max fps 30 is not ~30");

    // ---- pixels of the last copied frame ----
    ctx->CopyResource(staging.Get(), dst.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return Fail("Map");
    if (expect == "smooth") {
        const SceneEye l = Scene(m, 0, (int)info.leftWidth, (int)info.leftHeight);
        const SceneEye r = Scene(m, (int)info.leftWidth, (int)info.rightWidth, (int)info.rightHeight);
        ctx->Unmap(staging.Get(), 0);
        printf("  scene: marker left (%.2f, %.2f) %d px, right (%.2f, %.2f) %d px -> dx %+.2f dy %+.2f; darkest corner left %d, "
               "right %d\n", l.mx, l.my, l.n, r.mx, r.my, r.n, r.mx - l.mx, r.my - l.my, l.darkestCorner, r.darkestCorner);
        if (!l.marker || !r.marker) return Fail("the world marker is not visible in both eyes");
        if (std::fabs(r.mx - l.mx) > 1.5 || std::fabs(r.my - l.my) > 1.5)
            return Fail("the far marker is not at the same place in both halves (not a parallel symmetric re-projection)");
        if (l.darkestCorner < 20 || r.darkestCorner < 20) return Fail("a black corner: the output is not covered (edge guard)");
    } else if (expect == "overlays") {
        const int bad = CheckOverlays(m, (int)info.leftWidth, (int)info.leftHeight);
        ctx->Unmap(staging.Get(), 0);
        if (bad) return Fail("the overlay layers are not in the live frame as in a recording (or the indicator is)");
    } else {
        const int bad = CheckEye(m, 0, 0, (int)info.leftWidth, (int)info.leftHeight, tol) +
                        CheckEye(m, 1, (int)info.leftWidth, (int)info.rightWidth, (int)info.rightHeight, tol);
        ctx->Unmap(staging.Get(), 0);
        if (bad) return Fail("copied pixels differ from the test pattern");
    }

    // ---- a held claim: the producer must keep going around that slot ----
    c.Heartbeat(60, g_mode, g_size);
    uint32_t held = 0;
    uint64_t hv = 0;
    bool claimed = false;
    for (int i = 0; i < 200 && !claimed; ++i) { c.Heartbeat(60, g_mode, g_size); claimed = c.TestClaimLatest(&held, &hv); if (!claimed) Sleep(1); }
    if (!claimed) return Fail("could not claim the latest slot");
    LiveFrameInfo h0;
    c.Poll(&h0);
    int republished = 0, rewritten = 0;
    const ULONGLONG he = GetTickCount64() + 600;
    while (GetTickCount64() < he) {
        c.Heartbeat(60, g_mode, g_size);
        EyeCapLive s;
        if (EyeCapLiveRead(c.Block(), &s) && s.latestValue > hv && s.latestSlot == held) ++republished;
        if ((uint64_t)c.Block()->slotValue[held] != hv) ++rewritten;
        Sleep(1);
    }
    LiveFrameInfo h1;
    c.Poll(&h1);
    c.TestReleaseClaim(hv);
    printf("  claim held on slot %u (value %llu) for 0.6 s: producer published %llu frames meanwhile, re-published the held slot "
           "%d times, rewrote it %d times\n", held, (unsigned long long)hv, (unsigned long long)(h1.framesShared - h0.framesShared),
           republished, rewritten);
    if (h1.framesShared - h0.framesShared < 20) return Fail("the producer stalled while a slot was claimed");
    if (republished || rewritten) return Fail("the producer reused a slot the consumer was reading");

    // ---- switch to raw eyes: a new generation with the raw geometry ----
    if (switchRaw) {
        g_mode = EYECAP_LIVE_REPROJ_RAW;
        const uint32_t oldGen = info.generation;
        const ULONGLONG s0 = GetTickCount64();
        LiveFrameInfo ri;
        if (!WaitProducer(c, 6.0, &ri, oldGen)) return Fail("no new generation within 6 s after switching to raw eyes");
        printf("  switched to raw eyes: generation %u -> %u after %.2f s, %ux%u, eyes %ux%u + %ux%u, reprojected %u\n", oldGen,
               ri.generation, (GetTickCount64() - s0) / 1000.0, ri.width, ri.height, ri.leftWidth, ri.leftHeight, ri.rightWidth,
               ri.rightHeight, ri.reprojected);
        const uint32_t rawW = half ? (eyeW + 1) / 2 : eyeW, rawH = half ? (eyeH + 1) / 2 : eyeH;
        if (ri.reprojected != (half ? (uint32_t)EYECAP_LIVE_OUT_HALF : 0u) || ri.leftWidth != rawW || ri.rightWidth != rawW ||
            ri.leftHeight != rawH || ri.rightHeight != rawH)
            return Fail("after the switch the ring is not the raw eyes");
        d.Width = ri.width; d.Height = ri.height; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        ComPtr<ID3D11Texture2D> dst2;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &dst2))) return Fail("dst texture (raw)");
        const uint64_t c0 = c.FramesCopied();
        const ULONGLONG e = GetTickCount64() + 1000;
        while (GetTickCount64() < e) { c.Heartbeat(60, g_mode, g_size); c.Poll(nullptr); c.CopyLatest(dst2.Get()); Sleep(2); }
        printf("  raw eyes after the switch: %llu frames copied in 1 s\n", (unsigned long long)(c.FramesCopied() - c0));
        if (c.FramesCopied() - c0 < 30) return Fail("too few raw frames after the switch");
    }

    printf("LIVE_OK frames copied %llu, producer pid %u generation %u\n", (unsigned long long)c.FramesCopied(), info.producerPid,
           info.generation);
    fflush(stdout);
    if (abandon) TerminateProcess(GetCurrentProcess(), 0);   // no Close(): the producer must notice the stale heartbeat
    c.Close();
    return 0;
}
