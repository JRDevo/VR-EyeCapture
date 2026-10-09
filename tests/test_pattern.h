// test_pattern -- the known per-eye picture the self-tests draw (with clears only, no shaders) and verify.
//
// Per eye of size w x h (bands from the top):
//   A  [0,    h/4)   9 grey steps 0,32,...,224,255 (alpha 0 -- proves the encoder ignores alpha)
//   B  [h/4,  h/2)   red, green, blue, white
//   C  [h/2,  5h/8)  eye id: left = (200,40,40) red-ish, right = (40,40,200) blue-ish
//   D  [5h/8, 3h/4)  64-step horizontal ramp 0..252
//   E  [3h/4, h)     black with a 32 px white bar moving 8 px per frame
// Byte values are what lands in the texture (the RTV is the UNORM view): for an *_SRGB swapchain they are the
// sRGB-encoded picture, so the decoded video must show the same values (+-~3 after 4:2:0 + limited range).
#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

struct PatRect {
    RECT r;
    float c[4];
};

static const int kPatGrey[9] = {0, 32, 64, 96, 128, 160, 192, 224, 255};

inline void BuildPattern(std::vector<PatRect>& out, int eye, uint32_t frame, int ox, int oy, int w, int h) {
    out.clear();
    auto add = [&](int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
        PatRect p;
        p.r = {ox + x0, oy + y0, ox + x1, oy + y1};
        p.c[0] = r / 255.0f; p.c[1] = g / 255.0f; p.c[2] = b / 255.0f; p.c[3] = a / 255.0f;
        out.push_back(p);
    };
    add(0, 0, w, h, 32, 32, 32, 255);
    const int hA = h / 4, hB = h / 2, hC = 5 * h / 8, hD = 3 * h / 4;
    for (int i = 0; i < 9; ++i) add(i * w / 9, 0, (i + 1) * w / 9, hA, kPatGrey[i], kPatGrey[i], kPatGrey[i], 0);
    const int prim[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int i = 0; i < 4; ++i) add(i * w / 4, hA, (i + 1) * w / 4, hB, prim[i][0], prim[i][1], prim[i][2], 255);
    if (eye == 0) add(0, hB, w, hC, 200, 40, 40, 255);
    else add(0, hB, w, hC, 40, 40, 200, 255);
    for (int i = 0; i < 64; ++i) add(i * w / 64, hC, (i + 1) * w / 64, hD, i * 4, i * 4, i * 4, 255);
    add(0, hD, w, h, 0, 0, 0, 255);
    const int span = w > 64 ? w - 32 : 1;
    const int bx = (int)((frame * 8u) % (uint32_t)span);
    add(bx, hD, bx + 32, h, 255, 255, 255, 255);
}

// A WAV (48 kHz stereo 16-bit) with a 440 Hz tone, for PlaySound(SND_MEMORY) so WASAPI loopback is not silent.
inline std::vector<uint8_t> MakeToneWav(double seconds, double hz, double amp) {
    const uint32_t rate = 48000, frames = (uint32_t)(seconds * rate), dataBytes = frames * 4;
    std::vector<uint8_t> w(44 + dataBytes);
    auto put32 = [&](size_t o, uint32_t v) { memcpy(&w[o], &v, 4); };
    auto put16 = [&](size_t o, uint16_t v) { memcpy(&w[o], &v, 2); };
    memcpy(&w[0], "RIFF", 4); put32(4, 36 + dataBytes); memcpy(&w[8], "WAVE", 4);
    memcpy(&w[12], "fmt ", 4); put32(16, 16); put16(20, 1); put16(22, 2); put32(24, rate); put32(28, rate * 4);
    put16(32, 4); put16(34, 16); memcpy(&w[36], "data", 4); put32(40, dataBytes);
    for (uint32_t i = 0; i < frames; ++i) {
        const double s = amp * sin(2.0 * 3.14159265358979 * hz * i / rate);
        const int16_t v = (int16_t)(s * 32767.0);
        put16(44 + (size_t)i * 4, (uint16_t)v);
        put16(46 + (size_t)i * 4, (uint16_t)v);
    }
    return w;
}
