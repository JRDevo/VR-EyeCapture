// config -- config.ini parsing (key = value lines; '[sections]', ';' and '#' comments ignored).
//
// Lookup order: %APPDATA%\OpenXR-EyeCapture\config.ini if it exists, else config.ini next to the DLL.
// Unknown keys and bad values are logged and ignored (the default is kept).
#pragma once
#include <string>
#include <vector>
#include "dome.h"
#include "head_smoothing.h"
#include "hotkey.h"   // Hotkey, ParseHotkey (keyboard / mouse / XInput pad / DirectInput bindings)
#include "log.h"
#include "video_codec.h"


enum class GammaMode { Auto, Srgb, Linear };
// layout_3d: 3D layout of the files the tray makes from a flat recording (.mkv, 8K copy). tb_half = top-bottom half =
// YouTube's format; sbs_full = the recording as it is (the .mkv is then a plain remux). The recording itself is always full SBS.
enum class Layout3d { TbHalf, SbsHalf, SbsFull, TbFull };
// eye_crop: before scaling, each eye of a flat recording that is TALLER than this aspect is centre-cropped (vertically) to it.
// Flat SBS eyes are often taller than wide (4096x4630); a tall top/bottom video crashes the Quest YouTube player and YouTube
// caps it at 2160p. Wide169 (default) = 16:9 (8K frame 7680x4320), Std43 = 4:3 (5760x4320), None = keep the full eye,
// Pad43 = 4:3 crop, then black bars left and right up to 16:9 (7680x4320 with the 4:3 picture centred: YouTube's 8K tier
// without losing the taller view). Appended: the values are indexes in tests / the Settings code.
enum class EyeCrop { Wide169, Std43, None, Pad43 };
// post_when: when the tray converts a finished recording. now = at once; session_end = once no OpenXR session is live
// (the game left VR / destroyed its session); game_exit (default) = once the game process that made it has exited and no
// OpenXR game is live; manual = never on its own: finished recordings are queued and HELD until the user starts them
// (Settings queue "Start now", "Convert a recording...", tray menu "Convert last recording now"). Forced requests always run at once.
enum class PostWhen { Now, SessionEnd, GameExit, Manual };

// In-headset recording indicator ([indicator] section; see indicator.h).
struct IndicatorSettings {
    bool  enabled = true;                    // vr_indicator
    float x = 0.22f, y = -0.16f, z = -0.8f;  // indicator_x / _y / _z: centre in VIEW space, metres (-Z = ahead)
    float size = 0.02f;                      // indicator_size: edge length of the square quad, metres
    float opacity = 1.0f;                    // indicator_opacity: maximum alpha 0..1
};

struct Config {
    Hotkey       hotkey;                 // default Ctrl+Shift+F10
    std::wstring outputDir;              // default <dll folder>\captures
    VideoCodec   codec = VideoCodec::Hevc;   // codec = hevc | h264 | av1 (av1: hardware only, falls back to HEVC)
    unsigned     bitrateMbps = 0;        // 0 = codec default (120 HEVC / 80 H.264 / 80 AV1)
    unsigned     fps = 90;               // fallback when the display refresh cannot be detected
    unsigned     maxFps = 0;             // 0 = record every frame the game submits; else drop frames above this rate
    unsigned     maxWidth = 0;           // 0 = native (no scaling); else scale the SBS frame down to this width
    bool         audioEnabled = true;
    std::wstring audioDevice;            // substring of the render endpoint name; empty = default render device
    GammaMode    gamma = GammaMode::Auto;
    bool         requireTray = true;     // require_tray: layer stays dormant unless EyeCaptureTray.exe is running
    bool         swapEyes = false;       // swap_eyes: put the right eye in the left half (for players that flip SBS)
    bool         recordOverlays = true;  // record_overlays: composite the game's / mods' overlay layers into the recording
    // Post-processing, done by EyeCaptureTray.exe after each recording (the layer parses but ignores these).
    bool         postMkv = true;         // post_mkv: remux (sbs_full) or re-encode (other layout_3d) the MP4 to <name>.mkv with the Matroska stereo_mode tag
    bool         keepMp4 = false;        // keep_mp4: keep the MP4 after a verified MKV (and flat) conversion
    bool         postFlat = false;       // post_flat: also write <name>_FLAT.mp4 (left eye, 16:9, 3840 wide)
    bool         recordForYoutube = false;  // record_for_youtube: the layer records straight in the YouTube layout (layout_3d, eye_crop, short side 4320): the tray only remuxes
    bool         postYt8k = false;       // post_yt8k: also write <name>_YT8K.mkv (flat SBS only; layout per layout_3d)
    unsigned     yt8kBitrateMbps = 150;  // yt8k_bitrate_mbps: 8K step NVENC -b:v (maxrate 4/3 x, bufsize 2x); 20..400
    VideoCodec   yt8kCodec = VideoCodec::Hevc;   // yt8k_codec = hevc | av1: 8K step encoder (av1_nvenc; falls back to HEVC)
    PostWhen     postWhen = PostWhen::GameExit;  // post_when = now | session_end | game_exit | manual (tray only)
    Layout3d     layout3d = Layout3d::TbHalf;   // layout_3d = tb_half | sbs_half | sbs_full | tb_full (tray only; flat recordings)
    EyeCrop      eyeCrop = EyeCrop::Wide169;     // eye_crop = 16:9 | 4:3 | 4:3pad | none (tray only; flat recordings)
    std::wstring ffmpegPath;             // ffmpeg_path: empty = PATH, then WinGet packages, then <dll folder>\ffmpeg\ffmpeg.exe
    IndicatorSettings indicator;         // vr_indicator, indicator_x/_y/_z, indicator_size, indicator_opacity
    HeadSmoothingSettings head;          // head_smoothing, head_follow_deg, roll_lock, output_fov_scale, edge_guard, symmetric_fov
    DomeSettings dome;                   // [dome] projection, dome_width, dome_forward, fill, fill_fade_s, window_feather_deg
    LogLevel     logLevel = LogLevel::Info;
    std::wstring loadedFrom;             // path of the ini that was read ("" = defaults only)
};

// dllDir: folder of eye_capture.dll (no trailing slash). Always fills `out` with defaults first.
void LoadConfig(const std::wstring& dllDir, Config& out);
// "key=value;key=value" applied on top (same keys and checks as config.ini; each one logged). Used for the test-only
// environment variable XR_EYE_CAPTURE_CONFIG.
void ApplyConfigOverrides(const std::wstring& list, Config& c);
// Path LoadConfig would read (may not exist): %APPDATA% copy if present, else <dllDir>\config.ini.
std::wstring ConfigPath(const std::wstring& dllDir);
// Parses one ini file like LoadConfig (same rules, same logging) but starts from plain defaults and ALSO collects every
// "bad value" as a message ("line 12: bad value 'x' for key"). Returns true when the file was read with no problems.
// Used by the tray's Settings window to validate a candidate file before saving it.
bool ParseConfigFile(const std::wstring& path, Config& out, std::vector<std::wstring>& problems);
