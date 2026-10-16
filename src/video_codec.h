// video_codec -- the recording codec (config key `codec`), shared by config, the recorder, the layer and the tray.
#pragma once

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
