// yt_layout -- "Record for YouTube" (config key record_for_youtube): the frame geometry the layer records straight into,
// so the tray only remuxes (no re-encode). Header-only: used by the recorder, the layer and tests/head_smoothing_test.
//
// PredictYtLayout is a copy of the tray's pp::PredictLayout (tray/postprocess.h) -- the SAME arithmetic, so a YouTube-mode
// recording has exactly the frame the tray's 8K step would make from an SBS recording of 2 x eyeW x eyeH:
//   eye_crop  : ch = 2 floor(eyeW D / N / 2) if that is < eyeH (N:D = 16:9 / 4:3; none = no crop): centre crop, the eye
//               keeps its full width and loses height top and bottom equally
//   4:3pad    : the 4:3 crop, then black bars left and right up to 16:9 (eye units): pw = max(cw, 2 floor(ch 16/9 / 2 + 0.5)),
//               padX = 2 floor((pw - cw) / 4) (centred, even). Rounded to nearest, not floored: a Quest 3 eye (ch 3072)
//               gets pw 5462 -> 7680 wide (floored 5460 would give 7678). Every other crop: pw = cw, padX = 0.
//   scale     : sc = min(8192 / long side of one eye's packed area, shortSide / its short side) (shortSide 0 = native,
//               only the 8192 cap); the packed area of one eye is pw x ch, x2 wide for sbs_full, x2 tall for tb_full
//   eye in the frame: sw = 2 floor(pw sc / wDiv + 0.5), sh = 2 floor(ch sc / hDiv + 0.5), wDiv = 4 for sbs_half else 2,
//               hDiv = 4 for tb_half else 2 (the half layouts squeeze each eye by 2 on one axis); the picture inside it
//               (4:3pad): vw = 2 floor(cw sc / wDiv + 0.5) wide at vx = 2 floor(padX sc / wDiv + 0.5) (= sw / 0 otherwise)
//   frame     : sbs: 2 sw x sh (left | right), tb: sw x 2 sh (left on top)
// FitYtLayout then applies the recorder's limits on top: the long side <= the codec limit (8192 HEVC / AV1, 4096 H.264)
// and, when max_width is set, the frame width <= max_width -- one uniform factor, each eye's sw / sh floored to even
// (vw / vx with the same factor, even, inside sw).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "config.h"   // Layout3d, EyeCrop

// The 8K step's frame short side (YouTube 8K = 7680x4320 / 4320 tall).
const int kYtShortSide = 4320;

// pw = the eye's width after the 4:3pad bars (eye units, = cw otherwise); vw / vx = the picture's width / x offset
// inside the sw-wide eye in the frame (= sw / 0 unless padded).
struct YtLayout { int cw = 0, ch = 0, fw = 0, fh = 0, sw = 0, sh = 0, pw = 0, vw = 0, vx = 0; };

inline void YtEyeAspect(EyeCrop c, int& n, int& d) {   // the crop aspect: 4:3pad crops to 4:3 (then pads)
    const bool c43 = c == EyeCrop::Std43 || c == EyeCrop::Pad43;
    n = c == EyeCrop::Wide169 ? 16 : c43 ? 4 : 0;
    d = c == EyeCrop::Wide169 ? 9 : c43 ? 3 : 1;
}

// Identical results to pp::PredictLayout (tray/postprocess.h); see the header comment.
inline YtLayout PredictYtLayout(int eyeW, int eyeH, Layout3d lay, EyeCrop crop, int shortSide) {
    YtLayout o;
    o.cw = eyeW;
    o.ch = eyeH;
    int n, d;
    YtEyeAspect(crop, n, d);
    if (n > 0) {
        const int want = 2 * (int)std::floor((double)o.cw * d / n / 2.0);
        if (want < o.ch) o.ch = want;
    }
    o.pw = o.cw;
    if (crop == EyeCrop::Pad43) o.pw = std::max(o.cw, 2 * (int)std::floor((double)o.ch * 16 / 9 / 2.0 + 0.5));
    const int padX = 2 * ((o.pw - o.cw) / 4);
    const double ew = (double)(lay == Layout3d::SbsFull ? 2 * o.pw : o.pw), eh = (double)(lay == Layout3d::TbFull ? 2 * o.ch : o.ch);
    const double longCap = 8192.0 / (ew > eh ? ew : eh);
    double sc = longCap;
    if (shortSide > 0) {
        const double s = (double)shortSide / (ew < eh ? ew : eh);
        if (s < sc) sc = s;
    } else if (sc > 1.0) {
        sc = 1.0;
    }
    const double wDiv = lay == Layout3d::SbsHalf ? 4.0 : 2.0, hDiv = lay == Layout3d::TbHalf ? 4.0 : 2.0;
    o.sw = 2 * (int)std::floor(o.pw * sc / wDiv + 0.5);
    o.sh = 2 * (int)std::floor(o.ch * sc / hDiv + 0.5);
    o.vw = 2 * (int)std::floor(o.cw * sc / wDiv + 0.5);
    o.vx = 2 * (int)std::floor(padX * sc / wDiv + 0.5);
    const bool sbs = lay == Layout3d::SbsHalf || lay == Layout3d::SbsFull;
    o.fw = sbs ? 2 * o.sw : o.sw;
    o.fh = sbs ? o.sh : 2 * o.sh;
    return o;
}

inline bool YtTopBottom(Layout3d l) { return l == Layout3d::TbHalf || l == Layout3d::TbFull; }
inline const char* YtPackingKey(Layout3d l) {   // the layout_3d value
    return l == Layout3d::SbsHalf ? "sbs_half" : l == Layout3d::SbsFull ? "sbs_full" : l == Layout3d::TbFull ? "tb_full" : "tb_half";
}
inline const char* YtCropKey(EyeCrop c) {   // the eye_crop value
    return c == EyeCrop::Std43 ? "4:3" : c == EyeCrop::Pad43 ? "4:3pad" : c == EyeCrop::None ? "none" : "16:9";
}

// The recorded frame: the YtLayout scaled down (uniformly) to the encoder's limits.
struct YtFit {
    YtLayout native;            // PredictYtLayout's result
    int      sw = 0, sh = 0;    // one eye in the recorded frame
    int      vw = 0, vx = 0;    // its picture: width and x offset inside sw (4:3pad: narrower, centred; else sw / 0)
    int      fw = 0, fh = 0;    // the recorded frame
    double   factor = 1.0;      // recorded / native, <= 1
    bool     byCodec = false;   // the long side was over maxSide
    bool     byWidth = false;   // the width was over maxWidth
    bool Scaled() const { return byCodec || byWidth; }
};
// maxSide = VideoCodecMaxSide(codec); maxWidth = max_width (0 = none).
inline YtFit FitYtLayout(const YtLayout& l, Layout3d lay, unsigned maxSide, unsigned maxWidth) {
    YtFit r;
    r.native = l;
    r.sw = l.sw; r.sh = l.sh; r.fw = l.fw; r.fh = l.fh; r.vw = l.vw; r.vx = l.vx;
    const int longSide = std::max(l.fw, l.fh);
    double f = 1.0;
    if (maxSide && longSide > (int)maxSide) { f = std::min(f, (double)maxSide / longSide); r.byCodec = true; }
    if (maxWidth && l.fw > (int)maxWidth) { f = std::min(f, (double)maxWidth / l.fw); r.byWidth = true; }
    if (f >= 1.0) return r;
    auto even = [f](int v) { return std::max(2, 2 * (int)std::floor(v * f / 2.0 + 1e-7)); };
    r.sw = even(l.sw);
    r.sh = even(l.sh);
    // the picture: same factor, even, kept inside the eye (unpadded: exactly the eye)
    r.vw = l.vw >= l.sw ? r.sw : std::min(r.sw, even(l.vw));
    r.vx = l.vx <= 0 ? 0 : std::clamp(2 * (int)std::floor(l.vx * f / 2.0 + 0.5), 0, r.sw - r.vw);
    const bool sbs = !YtTopBottom(lay);
    r.fw = sbs ? 2 * r.sw : r.sw;
    r.fh = sbs ? r.sh : 2 * r.sh;
    r.factor = f;
    return r;
}

// Bilinear taps per axis for the re-projection pass rendering a srcW x srcH source eye area (the cropped symmetric eye
// at the source density) into sw x sh: the FitStageOutput rule applied to the LARGER minification of the two axes
// (an upscale on the other axis is plain bilinear): 1 up to 1.2x, else ceil(m / 2) clamped to 2..4.
inline uint32_t YtTaps(int srcW, int srcH, int sw, int sh) {
    const double mx = sw > 0 ? (double)srcW / sw : 1.0, my = sh > 0 ? (double)srcH / sh : 1.0;
    const double m = std::max(mx, my);
    return m <= 1.2 ? 1u : std::clamp<uint32_t>((uint32_t)std::ceil(m / 2.0), 2u, 4u);
}
