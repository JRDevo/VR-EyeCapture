// video_codec -- the recording codec (config key `codec`), shared by config, the recorder, the layer and the tray.
#pragma once
#include <cmath>

// codec = hevc (default) | h264 | av1. AV1 is hardware only (Media Foundation AV1 encoder MFT, e.g. NVIDIA RTX 40 / 50);
// when it cannot be set up the recorder falls back to HEVC (src/recorder.cpp, sink writer attempts).
enum class VideoCodec { Hevc, H264, Av1 };

// Config value ("hevc" / "h264" / "av1").
inline const char* VideoCodecKey(VideoCodec c) { return c == VideoCodec::H264 ? "h264" : c == VideoCodec::Av1 ? "av1" : "hevc"; }
// Display name ("HEVC" / "H.264" / "AV1").
inline const char* VideoCodecName(VideoCodec c) { return c == VideoCodec::H264 ? "H.264" : c == VideoCodec::Av1 ? "AV1" : "HEVC"; }
// bitrate_mbps = 0 picks this. AV1 80: AV1 needs roughly 30-40% less bitrate than HEVC for the same quality, so 80 Mbit/s
// AV1 ~ the 120 Mbit/s HEVC default (and the same as H.264's 80, which is the lowest default the files ever had).
inline unsigned VideoCodecDefaultMbps(VideoCodec c) { return c == VideoCodec::Hevc ? 120u : 80u; }
// Largest frame side the encoder takes (H.264 level 5.2 / NVENC: 4096; HEVC and AV1 NVENC: 8192).
inline unsigned VideoCodecMaxSide(VideoCodec c) { return c == VideoCodec::H264 ? 4096u : 8192u; }

// Largest frame AREA the standards allow (HEVC level 6.x MaxLumaPs, AV1 level 6.x MaxPicSize, H.264 level 6.x MaxFS x 256:
// all 35,651,584 = 8192 x 4352), counted on the coded size (both sides rounded up to 16). A larger frame (8064 x 4564 from a
// Half-Life: Alyx eye, 8192 x 4632 from ETS2 / ATS) encodes, but its stream claims no valid level and Windows players /
// hardware decoders refuse it or show only part of the picture.
constexpr unsigned long long kVideoCodecMaxArea = 35651584ull;
inline bool VideoCodecAreaOk(unsigned w, unsigned h) {
    return (unsigned long long)((w + 15u) & ~15u) * ((h + 15u) & ~15u) <= kVideoCodecMaxArea;
}
// Scales w x h (keeping the aspect, both even) until VideoCodecAreaOk. Returns false when nothing changed.
inline bool VideoCodecFitArea(unsigned& w, unsigned& h) {
    if (!w || !h || VideoCodecAreaOk(w, h)) return false;
    const double f = std::sqrt((double)kVideoCodecMaxArea / ((double)w * (double)h));
    unsigned nw = (unsigned)(w * f) & ~1u, nh = (unsigned)(h * f) & ~1u;
    while (!VideoCodecAreaOk(nw, nh) && nw > 64 && nh > 64) {   // the rounding up to 16 can still be a little over
        nw -= 2;
        nh = (unsigned)((unsigned long long)h * nw / w) & ~1u;
    }
    w = nw;
    h = nh;
    return true;
}

// rate_control = vbr (default) | cbr | qp: how the recorder's encoder spends bits (src/recorder.cpp, CreateWriter).
// vbr = unconstrained VBR at the mean bitrate (bitrate_mbps); cbr = constant bitrate at bitrate_mbps; qp = constant QP
// (Media Foundation "quality" mode with a fixed QP, config key qp 0..51; bitrate_mbps is ignored, the size follows the content).
enum class RateControl { Vbr, Cbr, Qp };
// Config value ("vbr" / "cbr" / "qp").
inline const char* RateControlKey(RateControl r) { return r == RateControl::Cbr ? "cbr" : r == RateControl::Qp ? "qp" : "vbr"; }
// Display name ("VBR" / "CBR" / "constant QP").
inline const char* RateControlName(RateControl r) { return r == RateControl::Cbr ? "CBR" : r == RateControl::Qp ? "constant QP" : "VBR"; }
