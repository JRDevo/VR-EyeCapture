// plugin -- obs-eyecapture.dll: the OBS Studio source "VR EyeCapture" (id eyecapture_source; the id never changes: saved
// scenes reference it). Shows the live eye images of a running VR game recorded by VR EyeCapture (src/live_share.h), through the
// consumer side in obs/live_consumer.cpp (protocol: src/live_share_proto.h). Build / install: obs/README.md.
//
// Threads (OBS): video_tick runs on the graphics thread OUTSIDE the graphics context -> it enters it (obs_enter_graphics)
// for the block / import work; video_render runs inside it. create / update / get_properties run on the UI thread,
// get_width / get_height anywhere (atomics). Every source shares ONE consumer per OBS process (one heartbeat, one claim,
// one copy of the whole SBS frame per new game frame into one texture); each source draws its own region of it.
// Nothing here waits on the CPU for the game: a frame that is not complete on the game's GPU yet is taken next frame.
//
// Heartbeat only while at least one source is SHOWING (preview or program): a source that is not visible anywhere
// costs the game nothing (the layer stops sharing 2 s after the last heartbeat).
//
// "Re-projection" (reproject): Same as recording (config.ini) = the layer re-projects the eyes like a recording
// (symmetric_fov / head smoothing of the game's config.ini, plus its overlay layers with record_overlays = 1, on the
// layer's own share device -- never on OBS's device); Raw eyes = the images as rendered. "Size" (size): Full, or Half =
// the layer renders the ring at half width and height (less VRAM, a 4x smaller copy here; the game-side copy stays full
// size). One ring per OBS process: the re-projection when any showing source wants it, Full when any wants it.
// "Zoom" (zoom, 100 .. 250 %, left / right eye views): a centred crop of the eye drawn scaled up to the source's size,
// here in the draw (gs_draw_sprite_subregion under a scale matrix) -- nothing changes in the game.
#include <obs-module.h>
#include <d3d11.h>
#include <atomic>
#include <mutex>
#include "live_consumer.h"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char* obs_module_name(void) { return "VR EyeCapture"; }
MODULE_EXPORT const char* obs_module_description(void) {
    return "Live eye images of a VR game recorded with VR EyeCapture (github.com/JRDevo/VR-EyeCapture)";
}

namespace {

enum View : int { kViewLeft = 0, kViewRight = 1, kViewSbs = 2 };

void ConsumerLog(int level, const char* msg) {
    blog(level == 0 ? LOG_ERROR : level == 1 ? LOG_WARNING : LOG_INFO, "[obs-eyecapture] %s", msg);
}

gs_color_format GsFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return GS_RGBA;
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return GS_BGRA;
    case DXGI_FORMAT_B8G8R8X8_UNORM:     return GS_BGRX;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return GS_R10G10B10A2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return GS_RGBA16F;
    default:                             return GS_UNKNOWN;
    }
}

// The 8-bit formats have an sRGB view (OBS creates it for gs_effect_set_texture_srgb); R10G10B10A2 / RGBA16F do not.
bool HasSrgbView(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM;
}

// ------------------------------------------------------------------------------------------------ shared consumer

struct Shared {
    LiveConsumer consumer;
    bool opened = false, giveUp = false;   // giveUp: not the D3D11 renderer / no usable device
    gs_texture_t* tex = nullptr;           // the whole SBS frame (plugin-owned, OBS's device)
    uint32_t texW = 0, texH = 0;
    DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t texPid = 0, texGen = 0;       // the producer generation the texture holds a frame of
    bool haveFrame = false;                // tex holds a frame of the current generation
    bool warnedFormat = false;
    LiveFrameInfo info;                    // valid when `live`
    bool live = false;
    unsigned wantFps = 0;                  // max of the showing sources since the last heartbeat
    unsigned modeVotes = 0;                // since the last heartbeat: bit 0 a showing source wants the re-projection, bit 1 raw
    uint32_t mode = EYECAP_LIVE_REPROJ_CONFIG;   // what the heartbeat writes (re-projection wins when sources disagree)
    unsigned sizeVotes = 0;                // since the last heartbeat: bit 0 a showing source wants full size, bit 1 half
    uint32_t size = EYECAP_LIVE_SIZE_FULL;       // what the heartbeat writes (full wins when sources disagree)
    bool polled = false;
    uint64_t lastFrame = 0;                // obs_get_video_frame_time of the last poll
};

std::mutex g_mx;            // g_refs / g_shared lifetime
int g_refs = 0;
Shared* g_shared = nullptr;

// Graphics context held.
void DestroyTexture(Shared& s) {
    if (s.tex) gs_texture_destroy(s.tex);
    s.tex = nullptr;
    s.texW = s.texH = 0;
    s.texFmt = DXGI_FORMAT_UNKNOWN;
    s.haveFrame = false;
}

// Graphics context held. Opens the block on OBS's D3D11 device the first time.
bool EnsureOpen(Shared& s) {
    if (s.opened) return true;
    if (s.giveUp) return false;
    if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
        blog(LOG_WARNING, "[obs-eyecapture] OBS is not using the Direct3D 11 renderer -- the VR EyeCapture source shows "
                          "nothing (Settings > Advanced > Video > Renderer)");
        s.giveUp = true;
        return false;
    }
    auto* dev = static_cast<ID3D11Device*>(gs_get_device_obj());
    if (!s.consumer.Open(dev, EYECAP_LIVE_SHM_NAME, ConsumerLog)) {
        blog(LOG_WARNING, "[obs-eyecapture] cannot open the live share block -- source disabled");
        s.giveUp = true;
        return false;
    }
    s.opened = true;
    blog(LOG_INFO, "[obs-eyecapture] live share block open, waiting for a game");
    return true;
}

// Graphics thread, from a showing source's tick: heartbeat + producer check (+ import / texture) + the copy of a newer
// frame, once per OBS frame. The copy happens here, not in video_render, so the source gets its size (and is drawn at
// all) as soon as the first frame is there.
void SharedTick(Shared& s, unsigned fps, bool raw, bool half) {
    if (fps > s.wantFps) s.wantFps = fps;
    s.modeVotes |= raw ? 2u : 1u;
    s.sizeVotes |= half ? 2u : 1u;
    // another source already did it this frame: the OBS frame time is the same for every tick of one frame (an exact
    // test at any OBS fps -- a GetTickCount64 window would cap the polling at ~64 per second, below a 120 / 144 fps OBS)
    const uint64_t frame = obs_get_video_frame_time();
    if (s.polled && frame == s.lastFrame) return;
    s.polled = true;
    s.lastFrame = frame;
    obs_enter_graphics();
    if (EnsureOpen(s)) {
        // one ring for all sources: the re-projection if any showing source wants it, raw eyes only if all of them do
        if (s.modeVotes) s.mode = (s.modeVotes & 1u) ? (uint32_t)EYECAP_LIVE_REPROJ_CONFIG : (uint32_t)EYECAP_LIVE_REPROJ_RAW;
        if (s.sizeVotes) s.size = (s.sizeVotes & 1u) ? (uint32_t)EYECAP_LIVE_SIZE_FULL : (uint32_t)EYECAP_LIVE_SIZE_HALF;
        if (s.consumer.Heartbeat(s.wantFps, s.mode, s.size)) {   // the showing sources since the last write
            s.wantFps = 0;
            s.modeVotes = 0;
            s.sizeVotes = 0;
        }
        LiveFrameInfo fi;
        s.live = s.consumer.Poll(&fi);
        if (s.live) {
            s.info = fi;
            const gs_color_format gf = GsFormat(fi.format);
            if (gf == GS_UNKNOWN) {
                if (!s.warnedFormat) {
                    s.warnedFormat = true;
                    blog(LOG_WARNING, "[obs-eyecapture] the game's eye format (DXGI %d) is not supported -- nothing shown", (int)fi.format);
                }
                s.live = false;
            } else if (!s.tex || s.texW != fi.width || s.texH != fi.height || s.texFmt != fi.format) {
                DestroyTexture(s);
                s.tex = gs_texture_create(fi.width, fi.height, gf, 1, nullptr, 0);
                if (s.tex) { s.texW = fi.width; s.texH = fi.height; s.texFmt = fi.format; }
                else { blog(LOG_ERROR, "[obs-eyecapture] gs_texture_create %ux%u failed", fi.width, fi.height); s.live = false; }
            }
            if (s.texPid != fi.producerPid || s.texGen != fi.generation) {   // a new ring: no frame of it yet
                s.texPid = fi.producerPid;
                s.texGen = fi.generation;
                s.haveFrame = false;
            }
            if (s.live && s.tex) {
                auto* dst = static_cast<ID3D11Texture2D*>(gs_texture_get_obj(s.tex));
                if (dst && s.consumer.CopyLatest(dst) == 1) s.haveFrame = true;
            }
        } else {
            s.haveFrame = false;
        }
    }
    obs_leave_graphics();
}

Shared* Acquire() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_shared) g_shared = new Shared();
    ++g_refs;
    return g_shared;
}

void Release() {
    Shared* gone = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (--g_refs == 0) { gone = g_shared; g_shared = nullptr; }
    }
    if (!gone) return;
    obs_enter_graphics();
    DestroyTexture(*gone);
    gone->consumer.Close();   // clears our heartbeat: the game stops sharing at its next frame
    obs_leave_graphics();
    delete gone;
}

// ------------------------------------------------------------------------------------------------ the source

struct Source {
    obs_source_t* src = nullptr;
    Shared* sh = nullptr;
    std::atomic<int> view{kViewLeft};
    std::atomic<bool> crop169{true};
    std::atomic<unsigned> maxFps{60};
    std::atomic<bool> raw{false};          // "Re-projection": false = same as recording (config.ini), true = raw eyes
    std::atomic<bool> half{false};         // "Size": false = full, true = half
    std::atomic<int> zoom{100};            // "Zoom" in percent, 100 .. 250 (left / right eye views)
    // the source's size (graphics thread writes, any thread reads) and the region of the shared texture drawn into it
    // (= the source size, or a centred part of it when zoomed; graphics thread only)
    std::atomic<uint32_t> cx{0}, cy{0};
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

const char* GetName(void*) { return "VR EyeCapture"; }

void Update(void* data, obs_data_t* settings) {
    auto* s = static_cast<Source*>(data);
    long long v = obs_data_get_int(settings, "view");
    s->view = (v == kViewRight || v == kViewSbs) ? (int)v : (int)kViewLeft;
    s->crop169 = obs_data_get_bool(settings, "crop169");
    long long f = obs_data_get_int(settings, "max_fps");
    s->maxFps = (unsigned)(f < 1 ? 1 : f > 240 ? 240 : f);
    s->raw = obs_data_get_int(settings, "reproject") == EYECAP_LIVE_REPROJ_RAW;
    s->half = obs_data_get_int(settings, "size") == EYECAP_LIVE_SIZE_HALF;
    long long z = obs_data_get_int(settings, "zoom");
    s->zoom = (int)(z < 100 ? 100 : z > 250 ? 250 : z);
}

void* Create(obs_data_t* settings, obs_source_t* source) {
    auto* s = new Source();
    s->src = source;
    s->sh = Acquire();
    Update(s, settings);
    return s;
}

void Destroy(void* data) {
    auto* s = static_cast<Source*>(data);
    Release();
    delete s;
}

void Defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, "view", kViewLeft);
    obs_data_set_default_bool(settings, "crop169", true);
    obs_data_set_default_int(settings, "max_fps", 60);
    obs_data_set_default_int(settings, "reproject", EYECAP_LIVE_REPROJ_CONFIG);
    obs_data_set_default_int(settings, "size", EYECAP_LIVE_SIZE_FULL);
    obs_data_set_default_int(settings, "zoom", 100);
}

obs_properties_t* Properties(void*) {
    obs_properties_t* props = obs_properties_create();
    obs_property_t* p = obs_properties_add_list(props, "view", "View", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, "Left eye", kViewLeft);
    obs_property_list_add_int(p, "Right eye", kViewRight);
    obs_property_list_add_int(p, "Side by side", kViewSbs);
    obs_properties_add_bool(props, "crop169", "Crop to 16:9 (center; left / right eye view)");
    obs_properties_add_int(props, "max_fps", "Max fps (the game copies at most this many frames per second)", 1, 240, 1);
    p = obs_properties_add_list(props, "reproject", "Re-projection", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, "Same as recording (config.ini)", EYECAP_LIVE_REPROJ_CONFIG);
    obs_property_list_add_int(p, "Raw eyes (off)", EYECAP_LIVE_REPROJ_RAW);
    p = obs_properties_add_list(props, "size", "Size", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, "Full", EYECAP_LIVE_SIZE_FULL);
    obs_property_list_add_int(p, "Half (less VRAM, smaller copy)", EYECAP_LIVE_SIZE_HALF);
    p = obs_properties_add_int_slider(props, "zoom", "Zoom (left / right eye view)", 100, 250, 5);
    obs_property_int_set_suffix(p, " %");
    return props;
}

uint32_t Width(void* data) { return static_cast<Source*>(data)->cx.load(); }
uint32_t Height(void* data) { return static_cast<Source*>(data)->cy.load(); }

// Centre-crop (x, y, w, h) to 16:9.
void Crop169(uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) {
    if (!w || !h) return;
    if ((uint64_t)w * 9 > (uint64_t)h * 16) {        // wider than 16:9: crop the sides
        const uint32_t cw = (uint32_t)((uint64_t)h * 16 / 9) & ~1u;
        x += (w - cw) / 2;
        w = cw;
    } else {                                          // taller (VR eyes usually are): crop top and bottom
        const uint32_t ch = (uint32_t)((uint64_t)w * 9 / 16) & ~1u;
        y += (h - ch) / 2;
        h = ch;
    }
}

// The shown region of the SBS frame: one eye (optionally zoomed, then centre-cropped to 16:9) or the whole frame. The
// source's size is the region WITHOUT the zoom (zooming does not resize the source in the scene); the drawn region is
// the zoomed one (a centred crop of the eye by 100 / zoom, then the 16:9 crop), scaled up to that size in Render.
void Region(Source* s, const LiveFrameInfo& fi) {
    uint32_t x = 0, y = 0, w = fi.width, h = fi.height;
    uint32_t zx = 0, zy = 0, zw = w, zh = h;
    const int v = s->view.load();
    if (v != kViewSbs) {
        if (v == kViewRight) { x = fi.leftWidth; w = fi.rightWidth; h = fi.rightHeight; }
        else { w = fi.leftWidth; h = fi.leftHeight; }
        const int z = s->zoom.load();
        zw = z > 100 ? (uint32_t)((uint64_t)w * 100 / (uint64_t)z) : w;
        zh = z > 100 ? (uint32_t)((uint64_t)h * 100 / (uint64_t)z) : h;
        if (!zw) zw = 1;
        if (!zh) zh = 1;
        zx = x + (w - zw) / 2;
        zy = y + (h - zh) / 2;
        if (s->crop169.load()) {
            Crop169(x, y, w, h);
            Crop169(zx, zy, zw, zh);
        }
    }
    s->x = zx;
    s->y = zy;
    s->w = zw;
    s->h = zh;
    s->cx = w;
    s->cy = h;
}

void Tick(void* data, float) {
    auto* s = static_cast<Source*>(data);
    if (!obs_source_showing(s->src)) return;   // not visible anywhere: no heartbeat from this source
    Shared& sh = *s->sh;
    SharedTick(sh, s->maxFps.load(), s->raw.load(), s->half.load());
    if (sh.live && sh.haveFrame) Region(s, sh.info);
    else if (!sh.live) { s->cx = 0; s->cy = 0; }
}

void Render(void* data, gs_effect_t*) {
    auto* s = static_cast<Source*>(data);
    const Shared& sh = *s->sh;
    if (!sh.live || !sh.haveFrame || !sh.tex) return;
    const uint32_t cx = s->cx.load(), cy = s->cy.load();
    if (!cx || !cy || !s->w || !s->h || s->x + s->w > sh.texW || s->y + s->h > sh.texH) return;
    // Colours as in the recording: sRGB-encoded 8-bit bytes are sampled through the texture's sRGB view (decoded to
    // linear), linear ones through the plain view; the framebuffer encodes. sRGB-encoded 10-bit / FP16 (gamma = srgb on
    // such a swapchain) has no sRGB view: plain view with the framebuffer encoding off, so the encoded values pass through
    // unchanged (the sRGB "view" there would sample them as linear and the framebuffer encode them twice: too bright).
    // Opaque: VR eye alpha is meaningless.
    const bool srgbView = sh.info.srgb && HasSrgbView(sh.texFmt);
    const bool passThrough = sh.info.srgb && !srgbView;
    gs_effect_t* e = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    const bool prev = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(!passThrough);
    gs_eparam_t* img = gs_effect_get_param_by_name(e, "image");
    if (srgbView) gs_effect_set_texture_srgb(img, sh.tex);
    else gs_effect_set_texture(img, sh.tex);
    const bool zoomed = s->w != cx || s->h != cy;   // a centred part of the eye, scaled up to the source size (bilinear)
    if (zoomed) {
        gs_matrix_push();
        gs_matrix_scale3f((float)cx / (float)s->w, (float)cy / (float)s->h, 1.0f);
    }
    while (gs_effect_loop(e, "Draw")) gs_draw_sprite_subregion(sh.tex, 0, s->x, s->y, s->w, s->h);
    if (zoomed) gs_matrix_pop();
    gs_enable_framebuffer_srgb(prev);
}

obs_source_info g_info;

} // namespace

bool obs_module_load(void) {
    g_info = obs_source_info{};
    g_info.id = "eyecapture_source";
    g_info.type = OBS_SOURCE_TYPE_INPUT;
    g_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
    g_info.get_name = GetName;
    g_info.create = Create;
    g_info.destroy = Destroy;
    g_info.get_width = Width;
    g_info.get_height = Height;
    g_info.get_defaults = Defaults;
    g_info.get_properties = Properties;
    g_info.update = Update;
    g_info.video_tick = Tick;
    g_info.video_render = Render;
    g_info.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
    obs_register_source(&g_info);
    blog(LOG_INFO, "[obs-eyecapture] loaded (live share protocol v%u)", (unsigned)EYECAP_LIVE_VERSION);
    return true;
}
