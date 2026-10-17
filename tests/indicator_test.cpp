// indicator_test -- CPU unit test of the in-headset indicator logic (src/indicator): state tracking from the recorder's
// states and the 64x64 straight-alpha picture. No GPU, no OpenXR runtime. Exit code 0 = all checks passed.
#include <cmath>
#include <cstdio>
#include <vector>
#include "indicator.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what, double v = 0) {
    printf("  %s %s (%.3f)\n", ok ? "ok  " : "FAIL", what, v);
    if (!ok) ++g_fail;
}
const uint8_t* Px(const std::vector<uint8_t>& b, int x, int y) { return &b[((size_t)y * kIndicatorPx + x) * 4]; }
} // namespace

int main() {
    // ---- state tracking ----
    {
        IndicatorTracker t;
        Check(t.Update(RecState::Idle, "", 0.0) == IndShow::Hidden, "idle -> hidden");
        Check(t.Update(RecState::Starting, "", 0.1) == IndShow::Starting, "starting -> amber");
        Check(t.Update(RecState::Recording, "", 0.5) == IndShow::Recording, "recording -> red pulse");
        Check(t.Update(RecState::Stopping, "", 5.0) == IndShow::Starting, "stopping (finalizing) -> amber");
        Check(t.NeedsMessage(RecState::Idle), "transition to idle asks for the result message");
        Check(t.Update(RecState::Idle, "saved C:\\x.mp4", 5.2) == IndShow::Saved, "saved -> green");
        Check(!t.NeedsMessage(RecState::Idle), "no message needed while staying idle");
        Check(t.Update(RecState::Idle, "", 6.6) == IndShow::Saved, "green still at +1.4 s");
        Check(t.Update(RecState::Idle, "", 6.8) == IndShow::Hidden, "hidden after 1.5 s");
        Check(t.Update(RecState::Idle, "", 60.0) == IndShow::Hidden, "stays hidden");
    }
    {
        IndicatorTracker t;
        t.Update(RecState::Starting, "", 0.0);
        Check(t.Update(RecState::Failed, "", 0.3) == IndShow::Failed, "start failed -> red X");
        Check(t.Update(RecState::Failed, "", 2.2) == IndShow::Failed, "red X at +1.9 s");
        Check(t.Update(RecState::Failed, "", 2.4) == IndShow::Hidden, "hidden after 2 s");
        t.Update(RecState::Starting, "", 10.0);
        t.Update(RecState::Recording, "", 10.2);
        Check(t.Update(RecState::Idle, "recording failed (no frames or finalize error)", 20.0) == IndShow::Failed,
              "finished without 'saved' -> red X");
        Check(t.Update(RecState::Starting, "", 20.5) == IndShow::Starting, "a new start replaces the timed state at once");
    }
    {   // indicator_live: violet while a live viewer receives frames, only when nothing else is shown (recording wins)
        IndicatorTracker t;
        Check(t.Update(RecState::Idle, "", 0.0, true) == IndShow::Live, "idle + live viewer -> live");
        Check(t.Update(RecState::Idle, "", 0.5, false) == IndShow::Hidden, "viewer gone -> hidden");
        Check(t.Update(RecState::Starting, "", 1.0, true) == IndShow::Starting, "starting wins over live");
        Check(t.Update(RecState::Recording, "", 1.5, true) == IndShow::Recording, "recording wins over live");
        Check(t.Update(RecState::Stopping, "", 5.0, true) == IndShow::Starting, "finalizing wins over live");
        Check(t.Update(RecState::Idle, "saved C:\\x.mp4", 5.2, true) == IndShow::Saved, "saved (green) wins over live");
        Check(t.Update(RecState::Idle, "", 6.8, true) == IndShow::Live, "after the green: live again");
    }

    // ---- picture ----
    std::vector<uint8_t> b((size_t)kIndicatorPx * kIndicatorPx * 4);
    RenderIndicator(IndShow::Recording, 0.25, 1.0f, true, false, b.data());   // sin peak: alpha 1.0
    const uint8_t* c = Px(b, 32, 32);
    Check(c[0] == 235 && c[1] == 35 && c[2] == 35 && c[3] == 255, "recording, pulse peak: centre red, opaque (RGBA)", c[3]);
    Check(Px(b, 0, 0)[3] == 0 && Px(b, 63, 63)[3] == 0, "corners transparent");
    Check(Px(b, 0, 0)[0] == 235, "straight alpha: colour kept where transparent (no dark fringe)");
    int partial = 0;
    for (size_t i = 3; i < b.size(); i += 4) if (b[i] > 0 && b[i] < 255) ++partial;
    Check(partial >= 60 && partial <= 400, "anti-aliased edge (partial-alpha ring around the disc)", partial);
    RenderIndicator(IndShow::Recording, 0.75, 1.0f, true, false, b.data());   // sin trough: alpha 0.35
    Check(std::abs(Px(b, 32, 32)[3] - 89) <= 1, "recording, pulse trough: alpha 0.35", Px(b, 32, 32)[3]);
    RenderIndicator(IndShow::Recording, 0.25, 0.5f, true, false, b.data());
    Check(std::abs(Px(b, 32, 32)[3] - 128) <= 1, "indicator_opacity 0.5 caps alpha", Px(b, 32, 32)[3]);
    RenderIndicator(IndShow::Starting, 0.75, 1.0f, true, true, b.data());
    c = Px(b, 32, 32);
    Check(c[0] == 0 && c[1] == 176 && c[2] == 255 && c[3] == 255, "starting: amber, steady (no pulse), BGRA order", c[3]);
    RenderIndicator(IndShow::Saved, 0.0, 1.0f, false, false, b.data());
    c = Px(b, 32, 32);
    Check(c[0] == 5 && c[1] == 156 && c[2] == 16 && c[3] == 255, "saved: green, linearised for a UNORM texture", c[1]);
    RenderIndicator(IndShow::Failed, 0.0, 1.0f, true, false, b.data());
    Check(Px(b, 32, 32)[3] == 255 && Px(b, 17, 17)[3] == 255 && Px(b, 46, 17)[3] == 255, "failed: X strokes opaque on both diagonals");
    Check(Px(b, 32, 12)[3] == 0 && Px(b, 12, 32)[3] == 0, "failed: transparent between the strokes (an X, not a disc)");
    RenderIndicator(IndShow::Live, 0.75, 1.0f, true, false, b.data());
    c = Px(b, 32, 32);
    Check(c[0] == 150 && c[1] == 60 && c[2] == 235 && c[3] == 255, "live: violet, steady (no pulse at the trough time)", c[3]);
    RenderIndicator(IndShow::Hidden, 0.0, 1.0f, true, false, b.data());
    int any = 0;
    for (size_t i = 3; i < b.size(); i += 4) any += b[i];
    Check(any == 0, "hidden: fully transparent");

    printf("=== indicator_test %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
