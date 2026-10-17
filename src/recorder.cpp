// recorder -- see recorder.h.
#include <initguid.h>   // CODECAPI_* GUIDs (selectany, no clash with mfuuid / strmiids)
#include "recorder.h"
#include <codecapi.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <strmif.h>     // ICodecAPI
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "audio_capture.h"
#include "disk.h"
#include "log.h"
#include "st3d.h"

namespace {

const UINT kRingSize = 3;   // shared SBS slots (game thread never waits; full ring = dropped frame)
const UINT kNv12Pool = 4;   // NV12 surfaces in flight inside the encoder

// Linear swapchain formats (UNORM / R10G10B10A2 / FP16 without _SRGB) hold linear light per the OpenXR spec. The D3D11
// video processor accepts a G10 input colour space but (measured on NVIDIA) does not apply the curve, so linear input
// is sRGB-encoded here first, on the private encoder device (no game state is touched).
const char kLinearToSrgbCs[] =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<unorm float4> dst : register(u0);\n"
    "float3 Enc(float3 l) {\n"
    "    l = saturate(l);\n"
    "    return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    dst[id.xy] = float4(Enc(src[id.xy].rgb), 1.0);\n"
    "}\n";

// Head-motion smoothing: re-project each eye of the SBS frame onto the smoothed head orientation (rotational
// timewarp). One full-screen triangle over the whole SBS texture; the pixel shader picks the eye by x. For an output
// pixel: ray in the smoothed (output) camera from the scaled fov -> C (actual-eye-from-smoothed rotation) -> project
// with the eye's actual fov -> sample that eye's half of the source bilinearly; outside it = black. The output is
// W x H (same layout as the source) or, with symmetric_fov, 2 x eyeW x eyeH of the common symmetric frustum -- either one
// scaled down to the recorded size when max_width / the codec limit asks for less (FitStageOutput). record_for_youtube
// (yt_layout.h): the two eyes of the symmetric frustum, vertically cropped (eye_crop), scaled and packed side by side or
// top / bottom into the YouTube frame -- the eye is picked by its destination rectangle (eye 1 = pos >= its x0 AND y0;
// for side-by-side y0 = 0, i.e. the old pick by x); eye_crop = 4:3pad renders the picture into a narrower rectangle
// (dst) centred in the eye's area, the rest of the area (uv outside 0..1) black. Source SRV and target RTV are the same typed UNORM / float format,
// so no gamma conversion happens. See head_smoothing.h.
const char kReprojVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
// misc.w = bilinear taps per axis (FitStageOutput): 1 = one bilinear sample (native size); n > 1 (the stage renders
// smaller than the source, max_width): an n x n grid over the output pixel's footprint in the source, from the same
// mapping evaluated one output pixel to the right / below, so a 4096 px eye shrunk to 640 px does not alias / shimmer.
// The footprint is per axis (dx, dy), so a layout that minifies one axis only (tb_half squeezes the height) is covered too.
const char kReprojPs[] =
    "struct Eye { float4 r0, r1, r2, outTan, srcTan, src, dst; };\n"   // src / dst: x0, y0, w, h in pixels
    "cbuffer P : register(b0) { Eye eye[2]; float4 misc; float4 misc2; };\n"   // misc: 1/srcW, 1/srcH, eye 1 x0 in the output, taps; misc2.x: eye 1 y0
    "Texture2D<float4> src : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float3 Ray(Eye E, float2 pos) {\n"   // output pixel -> ray in the actual eye camera
    "    float2 uv = (pos - E.dst.xy) / E.dst.zw;\n"
    "    float3 d = float3(lerp(E.outTan.x, E.outTan.y, uv.x), lerp(E.outTan.z, E.outTan.w, uv.y), -1.0);\n"
    "    return float3(dot(E.r0.xyz, d), dot(E.r1.xyz, d), dot(E.r2.xyz, d));\n"
    "}\n"
    "float2 SrcUv(Eye E, float3 a) {\n"   // ray (a.z < 0) -> normalized position in the eye's source image
    "    float2 t = a.xy / -min(a.z, -1e-6);\n"
    "    return float2((t.x - E.srcTan.x) / (E.srcTan.y - E.srcTan.x), (E.srcTan.z - t.y) / (E.srcTan.z - E.srcTan.w));\n"
    "}\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    const float4 black = float4(0.0, 0.0, 0.0, 1.0);\n"
    "    Eye E = eye[(pos.x >= misc.z && pos.y >= misc2.x) ? 1 : 0];\n"
    "    float2 uv = (pos.xy - E.dst.xy) / E.dst.zw;\n"
    "    if (any(uv < 0.0) || any(uv > 1.0)) return black;\n"   // outside the eye's picture (4:3pad bars; past the last row)
    "    float3 a = Ray(E, pos.xy);\n"
    "    if (a.z > -1e-6) return black;\n"
    "    float2 s = SrcUv(E, a);\n"
    "    if (any(s < 0.0) || any(s > 1.0)) return black;\n"
    "    float2 lo = E.src.xy + 0.5, hi = E.src.xy + E.src.zw - 0.5;\n"
    "    float2 p = E.src.xy + s * E.src.zw;\n"
    "    if (misc.w < 1.5) return float4(src.SampleLevel(smp, clamp(p, lo, hi) * misc.xy, 0).rgb, 1.0);\n"
    "    float2 dx = (SrcUv(E, Ray(E, pos.xy + float2(1.0, 0.0))) - s) * E.src.zw;\n"   // footprint in source pixels
    "    float2 dy = (SrcUv(E, Ray(E, pos.xy + float2(0.0, 1.0))) - s) * E.src.zw;\n"
    "    const int n = (int)misc.w;\n"
    "    float3 c = float3(0.0, 0.0, 0.0);\n"
    "    [loop] for (int j = 0; j < n; ++j) {\n"
    "        [loop] for (int i = 0; i < n; ++i) {\n"
    "            float2 o = (float2(i, j) + 0.5) / n - 0.5;\n"
    "            c += src.SampleLevel(smp, clamp(p + o.x * dx + o.y * dy, lo, hi) * misc.xy, 0).rgb;\n"
    "        }\n"
    "    }\n"
    "    return float4(c / (n * n), 1.0);\n"
    "}\n";

struct RpEye { float r0[4], r1[4], r2[4], outTan[4], srcTan[4], src[4], dst[4]; };
struct RpConstants { RpEye eye[2]; float misc[4]; float misc2[4]; };
static_assert(sizeof(RpConstants) % 16 == 0, "constant buffer size");

// VR180 / VR360 dome (see dome.h). Same full-screen triangle; the eye is picked by its destination rectangle (SBS for
// vr180, top-bottom for vr360). Per output pixel: equirect (lon, lat) -> dome ray -> M_e (= q_e^-1 * D) -> the eye
// camera -> coverage weight w (0 outside the eye's fov, 1 inside, ramping over window_feather_deg at the window edge,
// from the angular distance to the nearest fov edge plane) and a bilinear sample of that eye's half of the source.
// DOME_LAST_SEEN (fill = last_seen): MRT. RT0 = the output the video processor reads, RT1 / RT2 = the new dome colour /
// last-write time (ping-pong with t1 / t2 = the previous ones): dome' = lerp(dome, sample, w), time' = w >= 0.5 ? now :
// time, output = dome' * fade(now - time'). Without it (fill = black): output = sample * w.
const char kDomePs[] =
    "struct Eye { float4 r0, r1, r2, srcTan, src, dst; };\n"   // r0.w = 1: paint this eye this frame (usable pose)
    "cbuffer P : register(b0) { Eye eye[2]; float4 misc; float4 dome; };\n"   // misc: 1/srcW, 1/srcH; dome: lonSpan, now (s), fill_fade_s, feather (rad)
    "Texture2D<float4> src : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "#ifdef DOME_LAST_SEEN\n"
    "Texture2D<float4> oldDome : register(t1);\n"
    "Texture2D<float> oldTime : register(t2);\n"
    "struct Out { float4 o : SV_Target0; float4 d : SV_Target1; float t : SV_Target2; };\n"
    "#else\n"
    "struct Out { float4 o : SV_Target0; };\n"
    "#endif\n"
    "static const float kPi = 3.14159265358979;\n"
    "float EdgeDist(float3 n, float3 plane) { return dot(n, normalize(plane)); }\n"   // sin of the angular distance
    "Out main(float4 pos : SV_Position) {\n"
    "    const int e = (pos.x >= eye[1].dst.x && pos.y >= eye[1].dst.y) ? 1 : 0;\n"
    "    Eye E = eye[e];\n"
    "    float2 uv = (pos.xy - E.dst.xy) / E.dst.zw;\n"
    "    float lon = (uv.x - 0.5) * dome.x, lat = (0.5 - uv.y) * kPi;\n"
    "    float3 r = float3(cos(lat) * sin(lon), sin(lat), -cos(lat) * cos(lon));\n"
    "    float3 a = float3(dot(E.r0.xyz, r), dot(E.r1.xyz, r), dot(E.r2.xyz, r));\n"
    "    float w = 0.0;\n"
    "    float3 c = float3(0.0, 0.0, 0.0);\n"
    "    if (E.r0.w > 0.5 && a.z < -1e-6) {\n"
    "        float2 t = a.xy / -a.z;\n"
    "        float2 s = float2((t.x - E.srcTan.x) / (E.srcTan.y - E.srcTan.x), (E.srcTan.z - t.y) / (E.srcTan.z - E.srcTan.w));\n"
    "        if (all(s >= 0.0) && all(s <= 1.0)) {\n"
    "            w = 1.0;\n"
    "            if (dome.w > 0.0) {\n"
    "                float3 n = normalize(a);\n"
    "                float m = min(min(EdgeDist(n, float3(1.0, 0.0, E.srcTan.x)), EdgeDist(n, float3(-1.0, 0.0, -E.srcTan.y))),\n"
    "                              min(EdgeDist(n, float3(0.0, -1.0, -E.srcTan.z)), EdgeDist(n, float3(0.0, 1.0, E.srcTan.w))));\n"
    "                w = saturate(asin(saturate(m)) / dome.w);\n"
    "            }\n"
    "            float2 p = clamp(E.src.xy + s * E.src.zw, E.src.xy + 0.5, E.src.xy + E.src.zw - 0.5);\n"
    "            c = src.SampleLevel(smp, p * misc.xy, 0).rgb;\n"
    "        }\n"
    "    }\n"
    "    Out o;\n"
    "#ifdef DOME_LAST_SEEN\n"
    "    const int3 ip = int3(int2(pos.xy), 0);\n"
    "    float3 nd = lerp(oldDome.Load(ip).rgb, c, w);\n"
    "    float nt = w >= 0.5 ? dome.y : oldTime.Load(ip);\n"
    "    float fade = dome.z > 0.0 ? 1.0 - saturate((dome.y - nt - dome.z) / 2.0) : 1.0;\n"
    "    o.o = float4(nd * fade, 1.0);\n"
    "    o.d = float4(nd, 1.0);\n"
    "    o.t = nt;\n"
    "#else\n"
    "    o.o = float4(c * w, 1.0);\n"
    "#endif\n"
    "    return o;\n"
    "}\n";

struct DomeEyeCb { float r0[4], r1[4], r2[4], srcTan[4], src[4], dst[4]; };
struct DomeConstants { DomeEyeCb eye[2]; float misc[4]; float dome[4]; };
static_assert(sizeof(DomeConstants) % 16 == 0, "constant buffer size");
const float kDomeTimeReset = -1e9f;   // last-write time of never-painted dome pixels (always fully faded)

const UINT kTimingQueries = 6;

// ---- NV12 pool: tracked samples return their surface here when Media Foundation releases them ----
struct Nv12Pool {
    std::mutex m;
    std::condition_variable cv;
    std::vector<bool> busy;
    int Acquire(DWORD timeoutMs) {
        std::unique_lock<std::mutex> lk(m);
        int idx = -1;
        cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
            for (size_t i = 0; i < busy.size(); ++i) if (!busy[i]) { idx = (int)i; return true; }
            return false;
        });
        if (idx >= 0) busy[(size_t)idx] = true;
        return idx;
    }
    void Release(int i) {
        { std::lock_guard<std::mutex> lk(m); if (i >= 0 && (size_t)i < busy.size()) busy[(size_t)i] = false; }
        cv.notify_all();
    }
    bool AllFree(DWORD timeoutMs) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
            for (bool b : busy) if (b) return false;
            return true;
        });
    }
};

class PoolReturn final : public IMFAsyncCallback {
public:
    PoolReturn(std::shared_ptr<Nv12Pool> p, int i) : pool_(std::move(p)), idx_(i) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) { *ppv = static_cast<IMFAsyncCallback*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { const ULONG r = --ref_; if (!r) delete this; return r; }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult*) override { pool_->Release(idx_); return S_OK; }
private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<Nv12Pool> pool_;
    int idx_;
};

std::string GuidCategoryName(const GUID& g) {
    if (g == MFT_CATEGORY_VIDEO_ENCODER) return "video encoder";
    if (g == MFT_CATEGORY_VIDEO_PROCESSOR) return "video processor";
    if (g == MFT_CATEGORY_AUDIO_ENCODER) return "audio encoder";
    if (g == MFT_CATEGORY_AUDIO_EFFECT) return "audio effect";
    return "other";
}

std::string HrText(HRESULT hr) {
    char b[32];
    snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}

// CODECAPI_AVEncVideoEncodeQP / _FrameTypeQP (UINT64): bits 0-15 the default QP, 16-31 I, 32-47 P, 48-63 B frames.
UINT64 PackQp(unsigned q) {
    const UINT64 v = q > 51 ? 51 : q;
    return v | (v << 16) | (v << 32) | (v << 48);
}

// The video encoder MFT the sink writer created for `stream` (exists after SetInputMediaType), as ICodecAPI; null if none.
ComPtr<ICodecAPI> EncoderCodecApi(IMFSinkWriter* w, DWORD stream) {
    ComPtr<IMFSinkWriterEx> ex;
    if (!w || FAILED(w->QueryInterface(IID_PPV_ARGS(&ex)))) return nullptr;
    for (DWORD i = 0; i < 8; ++i) {
        GUID cat{};
        ComPtr<IMFTransform> t;
        if (FAILED(ex->GetTransformForStream(stream, i, &cat, &t)) || !t) break;
        if (cat != MFT_CATEGORY_VIDEO_ENCODER) continue;
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(t.As(&api))) return api;
    }
    return nullptr;
}
// "3", "0x0018001800180018" or "n/a" (GetValue failed / not supported)
std::string CodecApiGetText(ICodecAPI* api, const GUID& g) {
    VARIANT var;
    VariantInit(&var);
    if (!api || FAILED(api->GetValue(&g, &var))) return "n/a";
    char b[40];
    if (var.vt == VT_UI8 || var.vt == VT_I8) snprintf(b, sizeof(b), "0x%016llx", (unsigned long long)var.ullVal);
    else if (var.vt == VT_UI4 || var.vt == VT_I4 || var.vt == VT_UINT || var.vt == VT_INT) snprintf(b, sizeof(b), "%lu", (unsigned long)var.ulVal);
    else if (var.vt == VT_BOOL) snprintf(b, sizeof(b), "%s", var.boolVal ? "true" : "false");
    else snprintf(b, sizeof(b), "(vt %u)", (unsigned)var.vt);
    VariantClear(&var);
    return b;
}

// Highest AAC bitrate (bytes/s) the system AAC encoder accepts for 48 kHz stereo; 0 = no AAC encoder.
UINT32 ProbeAacBytesPerSecond() {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         nullptr, &outInfo, &acts, &n)) || n == 0)
        return 0;
    UINT32 best = 0;
    ComPtr<IMFTransform> t;
    if (SUCCEEDED(acts[0]->ActivateObject(IID_PPV_ARGS(&t)))) {
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(t->SetInputType(0, in.Get(), 0))) {
            for (UINT32 bps : {32000u, 24000u, 20000u, 16000u}) {
                ComPtr<IMFMediaType> out;
                MFCreateMediaType(&out);
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bps);
                if (SUCCEEDED(t->SetOutputType(0, out.Get(), MFT_SET_TYPE_TEST_ONLY))) { best = bps; break; }
            }
        }
        acts[0]->ShutdownObject();
    }
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return best;
}

} // namespace

const char* RecStateName(RecState s) {
    switch (s) {
    case RecState::Idle:      return "idle";
    case RecState::Starting:  return "starting";
    case RecState::Recording: return "recording";
    case RecState::Stopping:  return "stopping";
    default:                  return "failed";
    }
}

struct Recorder::Impl {
    Recorder* owner = nullptr;
    std::thread worker;

    // this recording
    IFrameSource* src = nullptr;
    RecorderSettings s;
    UINT W = 0, H = 0, leftW = 0, outW = 0, outH = 0, fps = 90;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool srgb = true;
    VideoCodec codec = VideoCodec::Hevc;   // the codec actually used (after a fallback)
    UINT bitrate = 0;
    UINT av1Speed = 0;                     // AV1: CODECAPI_AVEncCommonQualityVsSpeed set (0 = encoder default; see CreateWriter)
    int  qvsSet = -1;                      // CODECAPI_AVEncCommonQualityVsSpeed actually set (-1 = encoder default)
    std::string rcText;                    // rate control as applied ("VBR 120 Mb/s", "constant QP 24", ...; CreateWriter)
    // constant_frame_rate (worker thread): last written grid slot (-1 = none yet), the NV12 pool surface that holds the last
    // written frame (-1 = none: no repeat source), counters for the summary
    bool cfr = false;
    int64_t cfrLastSlot = -1;
    int cfrPrevIdx = -1;
    std::atomic<uint64_t> cfrRepeated{0}, cfrDropped{0};
    uint64_t cfrCapped = 0;                // gaps longer than 5 s (only their first 5 s were repeated)
    int64_t cfrRealSlot = -1;              // slot of the last frame that came from the game (the 5 s cap counts from it)
    bool cfrGapCapped = false;             // the current gap reached the cap (reset by the next game frame)

    // private encoder device (kept across recordings while the adapter is the same)
    ComPtr<ID3D11Device> enc;
    LUID encLuid{};
    ComPtr<ID3D11DeviceContext> encCtx;
    ComPtr<ID3D11DeviceContext4> encCtx4;
    SharedRing ring;
    bool ringCreated = false;

    // CPU side of the ring
    struct ItemEye { bool valid; float q[4]; float p[3]; float fov[4]; UINT x0, w, h; };
    struct Item {
        UINT slot; uint64_t ready; int64_t ts; int64_t disp; ItemEye eye[2];
        uint32_t nOv, nBehind; bool projUnpremul; OverlayDraw ov[kMaxOverlays];   // record_overlays (ov[0 .. nBehind) behind the projection)
    };
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Item> queue;
    std::vector<bool> slotBusy;
    std::vector<uint64_t> slotFreeValue;
    std::atomic<int> gameBusy{0};
    std::atomic<bool> stopReq{false};
    uint64_t freeCounter = 0;

    // video processor
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    std::vector<ComPtr<ID3D11VideoProcessorInputView>> inViews;
    std::vector<ComPtr<ID3D11Texture2D>> localCopies;   // only if the VP cannot read the shared textures directly
    // linear -> sRGB pre-pass (linear swapchain formats only)
    ComPtr<ID3D11ComputeShader> linCs;
    std::vector<ComPtr<ID3D11ShaderResourceView>> linSrv;
    ComPtr<ID3D11Texture2D> linTex;
    ComPtr<ID3D11UnorderedAccessView> linUav;
    ComPtr<ID3D11VideoProcessorInputView> linView;
    // head-motion smoothing (only when s.head.Enabled())
    std::atomic<bool> stage{false};    // re-projection stage on (head smoothing and / or symmetric fov); Start / worker init
    SymmetricFov sym;                  // symmetric_fov output (valid = on for this recording)
    UINT vpW = 0, vpH = 0, outRightX = 0;   // stage output = video processor input size; right eye x0 in it
    // flat re-projection rendered at the recorded size (max_width / codec limit, FitStageOutput): native stage output,
    // per-eye native and rendered widths, bilinear taps per axis (1 = the single tap of the native size)
    UINT rpNativeW = 0, rpNativeH = 0, rpTaps = 1;
    UINT rpNativeEyeW[2] = {0, 0}, rpEyeW[2] = {0, 0};
    bool rpScaled = false;
    // record_for_youtube (yt_layout.h): this recording is rendered in the YouTube layout (flat, symmetric fov only)
    bool yt = false;                   // on for this recording
    bool ytForcedSym = false;          // symmetric_fov = 0 was overridden for it
    int  ytWarn = 0;                   // asked for but off: 1 = dome projection, 2 = no usable view fov at record start
    bool ytTb = false;                 // top / bottom (tb_half, tb_full) else side by side
    int  ytShort = kYtShortSide;       // frame short side asked for (XR_EYE_CAPTURE_TEST_YT_SHORT overrides it)
    bool ytShortEnv = false;
    YtFit ytFit;                       // per-eye and frame size (after the codec / max_width limits)
    double ytTanV = 0;                 // output vertical tan after the crop (= sym.tanV * ch / eyeH)
    UINT outBottomY = 0;               // eye 1 y0 in the stage output (top / bottom layouts; 0 = side by side)
    HeadSmoother smoother;
    ComPtr<ID3D11VertexShader> rpVs;
    ComPtr<ID3D11PixelShader> rpPs;
    ComPtr<ID3D11Buffer> rpCb;
    ComPtr<ID3D11SamplerState> rpSmp;
    ComPtr<ID3D11Texture2D> rpTex;
    ComPtr<ID3D11RenderTargetView> rpRtv;
    ComPtr<ID3D11VideoProcessorInputView> rpView;
    std::vector<ComPtr<ID3D11ShaderResourceView>> rpRingSrv;   // sRGB path: the ring slots
    ComPtr<ID3D11ShaderResourceView> rpLinSrv;                 // linear path: the pre-pass output
    struct TsQuery { ComPtr<ID3D11Query> disjoint, begin, end; bool pending = false; };
    std::vector<TsQuery> rpQ;
    UINT rpQi = 0;
    // VR180 / VR360 dome (projection != flat; replaces the symmetric / smoothing re-projection)
    DomeSize dome;                     // valid = dome mode for this recording
    bool domeHaveFrame = false;        // D fixed (first frame with a usable pose)
    DomeFrame domeFrame;
    bool domeLastSeen = false;         // fill = last_seen: persistent ping-pong dome
    bool domeClear = false;            // clear the dome at the next frame (inside the device lock)
    int domeCur = 0;                   // index of the current (read) dome / time texture
    ComPtr<ID3D11Texture2D> domeTex[2], domeTimeTex[2];
    ComPtr<ID3D11RenderTargetView> domeRtv[2], domeTimeRtv[2];
    ComPtr<ID3D11ShaderResourceView> domeSrv[2], domeTimeSrv[2];
    uint64_t domePainted = 0, domeNoPose = 0;
    // record_overlays (overlay.h): one atlas per format family, created by the worker at the first frame that has a
    // layer of that family (bit f = OverlayFamily f)
    std::atomic<uint32_t> ovWantMask{0}, ovReadyMask{0}, ovFailedMask{0};
    std::vector<ComPtr<ID3D11Texture2D>> ovAtlas[kOverlayFamilies];
    OverlayCompositor ovComp;
    std::atomic<uint64_t> ovLayersIn{0}, ovNotReady{0}, ovFull{0}, ovSetupFailed{0}, ovBehind{0};   // game thread counts
    uint64_t ovFrames = 0, ovDraws = 0;    // worker
    bool ovLoggedFull = false, ovLoggedFirst = false;   // game thread / worker (one each)
    void InitOverlays();
    // smoothing stats (worker thread only)
    uint64_t smFrames = 0, smGuarded = 0, smUncovered = 0, smCatch = 0, smNoPose = 0, smResets = 0, gpuN = 0;
    double smCorrSum = 0, smCorrMax = 0, gpuSum = 0, gpuMax = 0, cpuSum = 0, cpuMax = 0;
    HANDLE devHandle = nullptr;   // IMFDXGIDeviceManager handle, to lock the device around our GPU work
    std::vector<ComPtr<ID3D11Texture2D>> nv12;
    std::vector<ComPtr<ID3D11VideoProcessorOutputView>> outViews;
    std::vector<ComPtr<IMFAsyncCallback>> poolCb;
    std::shared_ptr<Nv12Pool> pool;

    // writer
    ComPtr<IMFDXGIDeviceManager> devMgr;
    UINT resetToken = 0;
    std::mutex wm;
    ComPtr<IMFSinkWriter> writer;
    DWORD vStream = 0, aStream = 0;
    bool hasAudio = false;
    UINT32 aacBytes = 0;
    AudioCapture audio;

    // timing (t0 = first video frame on the QPC clock)
    std::atomic<int64_t> t0{INT64_MIN};
    std::atomic<int64_t> t0Disp{0};  // displayTime (ns) of the frame at t = 0
    bool loggedFirst = false;        // worker
    int64_t lastTs = INT64_MIN;      // game thread
    int64_t nextDue = INT64_MIN;     // game thread (max_fps)
    std::atomic<int64_t> lastWrittenTs{0};

    // stats
    std::atomic<uint64_t> written{0}, dropRing{0}, dropGpu{0}, dropPool{0}, dropDup{0}, decimated{0}, audioFrames{0};
    std::atomic<uint64_t> writeErrors{0};
    std::mutex statusLock;
    std::string message;
    ULONGLONG startTick = 0;

    void Run();
    bool Init();
    bool FitOutputSize();
    bool InitVideoProcessor();
    bool InitLinearPass();
    bool InitReprojection(DXGI_FORMAT vpFmt);
    bool InitDomeTextures(DXGI_FORMAT vpFmt, double& mb);
    void Reproject(ID3D11ShaderResourceView* const* srvs, UINT nSrv, ID3D11RenderTargetView* const* rtvs, UINT nRtv,
                   const void* cb, size_t cbSize);
    void DomeConstantsFor(const Item& it, DomeConstants& dc);
    std::wstring SidecarPath() const;
    void WriteSidecar(const std::string& file);
    void ReadTimings();
    HRESULT CreateWriter(VideoCodec c, bool hardware, std::string& why);
    void LogTransforms();
    void Process(const Item& it);
    bool WriteVideoSample(int n, int64_t time);
    void CfrFill(int64_t slot);
    void CfrIdleFill();
    void FreeSlot(UINT slot, uint64_t freeValue, bool updateValue);
    void OnAudio(const int16_t* pcm, uint32_t frames, int64_t startQpc);
    void Cleanup();
    void SetMessage(const std::string& m) { std::lock_guard<std::mutex> lk(statusLock); message = m; }
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }

Recorder::~Recorder() {
    Stop();
    WaitIdle(30000);
    if (impl_->worker.joinable()) impl_->worker.detach();   // only if finalize hung for 30 s
}

bool Recorder::Start(IFrameSource* src, const RecorderSettings& s, UINT w, UINT h, UINT leftWidth, DXGI_FORMAT fmt, bool srgbEncoded) {
    const RecState st = state_.load();
    if (st == RecState::Starting || st == RecState::Recording || st == RecState::Stopping) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    Impl& m = *impl_;
    m.src = src;
    m.s = s;
    m.W = w; m.H = h; m.leftW = leftWidth; m.fmt = fmt; m.srgb = srgbEncoded;
    m.fps = s.fps ? s.fps : 90;
    if (s.maxFps && s.maxFps < m.fps) m.fps = s.maxFps;
    m.codec = s.codec;
    m.stopReq = false;
    m.t0 = INT64_MIN;
    m.lastTs = INT64_MIN;
    m.nextDue = INT64_MIN;
    m.lastWrittenTs = 0;
    m.written = 0; m.dropRing = 0; m.dropGpu = 0; m.dropPool = 0; m.dropDup = 0; m.decimated = 0; m.audioFrames = 0; m.writeErrors = 0;
    m.cfr = s.constantFrameRate;
    m.cfrLastSlot = -1; m.cfrPrevIdx = -1; m.cfrRepeated = 0; m.cfrDropped = 0; m.cfrCapped = 0;
    m.cfrRealSlot = -1; m.cfrGapCapped = false;
    m.qvsSet = -1; m.rcText.clear();
    m.t0Disp = 0;
    m.loggedFirst = false;
    m.dome = DomeSize();
    if (s.dome.Enabled())   // the dome replaces symmetric_fov / head smoothing
        m.dome = ComputeDomeSize(s.dome.projection, s.dome.width, s.startFovValid, s.startFov[0], s.startFov[1],
                                 s.startEyeW[0], s.startEyeW[1]);
    m.domeHaveFrame = false;
    m.domeLastSeen = m.dome.valid && s.dome.fill == DomeFill::LastSeen;
    m.domeClear = false;
    m.domeCur = 0;
    m.domePainted = m.domeNoPose = 0;
    m.ovWantMask = 0; m.ovReadyMask = 0; m.ovFailedMask = 0;
    m.ovLayersIn = 0; m.ovNotReady = 0; m.ovFull = 0; m.ovSetupFailed = 0; m.ovBehind = 0; m.ovFrames = 0; m.ovDraws = 0;
    m.ovLoggedFull = m.ovLoggedFirst = false;
    m.sym = SymmetricFov();
    // record_for_youtube: flat recordings only, and it needs the symmetric frustum (turns symmetric_fov on for this recording)
    const bool ytWanted = s.youtube && !m.dome.valid;
    if (!m.dome.valid && (s.head.symmetricFov || ytWanted) && s.startFovValid)
        m.sym = ComputeSymmetricFov(s.startFov[0], s.startFov[1], s.startEyeW[0], s.startEyeH[0], s.startEyeW[1], s.startEyeH[1]);
    m.yt = ytWanted && m.sym.valid;
    m.ytWarn = (s.youtube && m.dome.valid) ? 1 : (ytWanted && !m.sym.valid) ? 2 : 0;
    m.ytForcedSym = m.yt && !s.head.symmetricFov;
    m.stage = m.dome.valid || s.head.Enabled() || m.sym.valid;
    m.smoother.Configure(s.head);   // also resets the filter: every recording starts at the actual head pose
    m.smoother.SetSymmetricOutput(m.sym.valid ? m.sym.tanH : 0.0, m.sym.valid ? m.sym.tanV : 0.0);
    m.vpW = m.dome.valid ? m.dome.frameW : m.sym.valid ? 2 * m.sym.eyeW : w;
    m.vpH = m.dome.valid ? m.dome.frameH : m.sym.valid ? m.sym.eyeH : h;
    m.outRightX = m.dome.valid ? m.dome.eyeW : m.sym.valid ? m.sym.eyeW : leftWidth;
    // Flat re-projection (symmetric_fov / head smoothing): render the stage output at the size that is recorded
    // (max_width, the codec limit) instead of rendering it at full size and letting the video processor scale it -- the
    // pass then shades only the recorded pixels. The shader works in normalized coordinates of each eye's output
    // rectangle, so only the rectangles shrink; overlays are composited into the ring (source size) before it. The dome
    // keeps its own size (dome_width) and is scaled by the video processor as before.
    m.rpNativeW = m.vpW; m.rpNativeH = m.vpH; m.rpTaps = 1; m.rpScaled = false;
    m.rpNativeEyeW[0] = m.rpEyeW[0] = m.outRightX;
    m.rpNativeEyeW[1] = m.rpEyeW[1] = m.vpW - m.outRightX;
    m.ytTb = false; m.ytShort = kYtShortSide; m.ytShortEnv = false; m.ytFit = YtFit(); m.ytTanV = 0; m.outBottomY = 0;
    if (m.yt) {
        // YouTube layout: the symmetric eye (sym.eyeW x sym.eyeH at the source density) centre-cropped to eye_crop, scaled
        // and packed exactly as the tray's 8K step would (PredictYtLayout), then the codec / max_width limits (FitYtLayout).
        // The pass renders straight into that frame (vpW x vpH = the encoder size): the video processor does not scale.
        wchar_t ev[16];
        const DWORD n = GetEnvironmentVariableW(L"XR_EYE_CAPTURE_TEST_YT_SHORT", ev, 16);
        if (n > 0 && n < 16) {
            const int v = _wtoi(ev);
            if (v >= 2 && v <= 16384) { m.ytShort = v; m.ytShortEnv = true; }
        }
        const YtLayout yl = PredictYtLayout((int)m.sym.eyeW, (int)m.sym.eyeH, s.layout3d, s.eyeCrop, m.ytShort);
        m.ytFit = FitYtLayout(yl, s.layout3d, VideoCodecMaxSide(s.codec), s.maxWidth);
        m.ytTb = YtTopBottom(s.layout3d);
        m.ytTanV = m.sym.tanV * (double)yl.ch / (double)m.sym.eyeH;   // the crop: a symmetric subset of the vertical range
        m.smoother.SetSymmetricOutput(m.sym.tanH, m.ytTanV);          // edge guard / coverage test on the cropped frustum
        m.vpW = (UINT)m.ytFit.fw; m.vpH = (UINT)m.ytFit.fh;
        m.outRightX = m.ytTb ? 0u : (UINT)m.ytFit.sw;
        m.outBottomY = m.ytTb ? (UINT)m.ytFit.sh : 0u;
        m.rpEyeW[0] = m.rpEyeW[1] = (UINT)m.ytFit.sw;
        m.rpTaps = YtTaps((int)m.sym.eyeW, yl.ch, m.ytFit.vw, m.ytFit.sh);   // the picture's width (4:3pad: < sw)
    }
    if (m.stage && !m.dome.valid && !m.yt) {
        const UINT codecMax = VideoCodecMaxSide(s.codec);
        const UINT maxW = s.maxWidth ? std::min<UINT>(s.maxWidth, codecMax) : codecMax;
        const StageFit fit = FitStageOutput(m.rpNativeEyeW[0], m.rpNativeEyeW[1], m.vpH, maxW, codecMax);
        if (fit.scaled) {
            m.rpScaled = true;
            m.rpEyeW[0] = fit.eyeW[0]; m.rpEyeW[1] = fit.eyeW[1];
            m.rpTaps = fit.taps;
            m.vpW = fit.eyeW[0] + fit.eyeW[1];
            m.vpH = fit.h;
            m.outRightX = fit.eyeW[0];
        }
    }
    m.smFrames = m.smGuarded = m.smUncovered = m.smCatch = m.smNoPose = m.smResets = m.gpuN = 0;
    m.smCorrSum = m.smCorrMax = m.gpuSum = m.gpuMax = m.cpuSum = m.cpuMax = 0;
    m.queue.clear();
    m.SetMessage("");
    m.startTick = GetTickCount64();
    state_ = RecState::Starting;
    m.worker = std::thread([this] { impl_->Run(); });
    return true;
}

bool Recorder::SubmitFrame(int64_t ts, const EyeView& L, const EyeView& R, int64_t displayTimeNs, const OverlayFrame* ov) {
    Impl& m = *impl_;
    m.gameBusy.fetch_add(1);
    struct Guard { std::atomic<int>& c; ~Guard() { c.fetch_sub(1); } } guard{m.gameBusy};
    if (state_.load() != RecState::Recording) return false;
    if (m.lastTs != INT64_MIN && ts <= m.lastTs) { ++m.dropDup; return false; }
    if (m.s.maxFps) {
        const int64_t period = 10000000LL / m.s.maxFps;
        if (m.nextDue != INT64_MIN && ts < m.nextDue - period / 4) { ++m.decimated; return false; }
        m.nextDue = (m.nextDue == INT64_MIN || ts > m.nextDue + period) ? ts + period : m.nextDue + period;
    }
    UINT slot = 0;
    uint64_t waitFree = 0;
    {
        std::lock_guard<std::mutex> lk(m.qm);
        size_t i = 0;
        for (; i < m.slotBusy.size(); ++i) if (!m.slotBusy[i]) break;
        if (i == m.slotBusy.size()) { ++m.dropRing; return false; }
        m.slotBusy[i] = true;
        slot = (UINT)i;
        waitFree = m.slotFreeValue[i];
    }
    EyeView l = L, r = R;
    l.w = std::min<uint32_t>(l.w, m.leftW);
    l.h = std::min<uint32_t>(l.h, m.H);
    r.w = std::min<uint32_t>(r.w, m.W - m.leftW);
    r.h = std::min<uint32_t>(r.h, m.H);
    // record_overlays: pack this frame's overlay sub-images into the slot's atlas of their format family (shelf packing
    // per family, identical sub-images once, a cube layer's 6 faces as one 3 x 2 block). Nothing at all happens without
    // overlays. Layers of a family whose atlas the worker has not made yet are only counted (and the atlas requested).
    OverlayCopy copies[kMaxOverlayCopies];
    OverlayDraw draws[kMaxOverlays];
    UINT nCopy = 0;
    uint32_t nDraw = 0, nDrawBehind = 0;
    if (ov && ov->n) {
        const uint32_t n = std::min<uint32_t>(ov->n, kMaxOverlays);
        m.ovLayersIn += n;
        const uint32_t readyMask = m.ovReadyMask.load(std::memory_order_acquire), failedMask = m.ovFailedMask.load();
        struct Shelf { UINT cx = 0, cy = 0, rowH = 0; } shelf[kOverlayFamilies];
        const UINT A = kOverlayAtlasSize;
        for (uint32_t i = 0; i < n; ++i) {
            const OverlayLayer& la = ov->layer[i];
            if (la.family < 0 || la.family >= kOverlayFamilies) continue;
            const uint32_t bit = 1u << la.family;
            if (failedMask & bit) { ++m.ovSetupFailed; continue; }
            if (!(readyMask & bit)) { m.ovWantMask.fetch_or(bit); ++m.ovNotReady; continue; }
            const bool cube = la.kind == OverlayKind::Cube;
            const UINT bw = cube ? 3 * la.w : la.w, bh = cube ? 2 * la.h : la.h, nc = cube ? 6 : 1;
            OverlayDraw& d = draws[nDraw];
            d.l = la;
            bool placed = false;
            for (uint32_t j = 0; j < nDraw && !placed; ++j) {   // the same sub-image again (e.g. one per eye): reuse it
                const OverlayLayer& o = draws[j].l;
                if (o.image == la.image && o.arrayIndex == la.arrayIndex && o.x == la.x && o.y == la.y && o.w == la.w &&
                    o.h == la.h && (o.kind == OverlayKind::Cube) == cube) {
                    d.ax = draws[j].ax; d.ay = draws[j].ay; placed = true;
                }
            }
            if (!placed) {
                Shelf& s = shelf[la.family];
                if (s.cx + bw > A) { s.cx = 0; s.cy += s.rowH; s.rowH = 0; }
                if (bw > A || s.cy + bh > A || nCopy + nc > kMaxOverlayCopies) {
                    ++m.ovFull;
                    if (!m.ovLoggedFull) {
                        m.ovLoggedFull = true;
                        LOGW("recorder: overlay sub-image %ux%u (%s) does not fit the %ux%u %s overlay atlas next to this frame's "
                             "other overlays -- not recorded on such frames", bw, bh, OverlayKindName(la.kind), A, A,
                             OverlayFamilyName(la.family));
                    }
                    continue;
                }
                d.ax = s.cx; d.ay = s.cy;
                for (UINT f = 0; f < nc; ++f) {
                    OverlayCopy& c = copies[nCopy++];
                    c.image = la.image; c.sc = la.sc; c.scKey = la.scKey; c.family = la.family;
                    c.arrayIndex = la.arrayIndex + f;
                    c.x = la.x; c.y = la.y; c.w = la.w; c.h = la.h;
                    c.dstX = s.cx + (f % 3) * la.w; c.dstY = s.cy + (f / 3) * la.h;
                }
                s.cx += bw;
                s.rowH = std::max(s.rowH, bh);
            }
            if (la.behind) { ++nDrawBehind; ++m.ovBehind; }   // behind layers come first in ov->layer
            ++nDraw;
        }
    }
    const uint64_t ready = m.src->CopyEyes(slot, l, r, m.leftW, waitFree, nCopy ? copies : nullptr, nCopy);
    if (!ready) {
        m.FreeSlot(slot, 0, false);
        ++m.dropGpu;
        return false;
    }
    const int64_t disp = displayTimeNs ? displayTimeNs : ts * 100;
    int64_t expected = INT64_MIN;
    if (m.t0.compare_exchange_strong(expected, ts)) m.t0Disp = disp;
    m.lastTs = ts;
    Impl::Item item{};
    item.slot = slot; item.ready = ready; item.ts = ts; item.disp = disp;
    item.nOv = nDraw;
    item.nBehind = nDrawBehind;
    item.projUnpremul = ov && ov->projUnpremultiplied;
    for (uint32_t i = 0; i < nDraw; ++i) item.ov[i] = draws[i];
    if (m.stage || nDraw) {
        const EyeView* ev[2] = {&l, &r};
        const UINT x0[2] = {0, m.leftW};
        for (int e = 0; e < 2; ++e) {
            Impl::ItemEye& ie = item.eye[e];
            const EyeView& v = *ev[e];
            ie.x0 = x0[e]; ie.w = v.w; ie.h = v.h;
            // usable pose: finite fov, left < right, down < up, all within +-89 degrees
            bool ok = v.hasPose;
            for (int i = 0; ok && i < 4; ++i) ok = std::isfinite(v.fov[i]) && std::fabs(v.fov[i]) < 1.553f && std::isfinite(v.orientation[i]);
            ok = ok && v.fov[0] < v.fov[1] && v.fov[3] < v.fov[2];
            ie.valid = ok;
            memcpy(ie.q, v.orientation, sizeof(ie.q));
            memcpy(ie.p, v.position, sizeof(ie.p));
            memcpy(ie.fov, v.fov, sizeof(ie.fov));
        }
    }
    {
        std::lock_guard<std::mutex> lk(m.qm);
        m.queue.push_back(item);
    }
    m.qcv.notify_one();
    return true;
}

void Recorder::Stop() {
    Impl& m = *impl_;
    m.stopReq = true;
    RecState e = RecState::Recording;
    if (!state_.compare_exchange_strong(e, RecState::Stopping)) {
        e = RecState::Starting;
        state_.compare_exchange_strong(e, RecState::Stopping);
    }
    m.qcv.notify_all();
}

bool Recorder::WaitIdle(DWORD ms) {
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const RecState st = state_.load();
        if (st == RecState::Idle || st == RecState::Failed) break;
        if (GetTickCount64() - t0 >= ms) return false;
        Sleep(5);
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    return true;
}

RecorderStatus Recorder::Status() {
    Impl& m = *impl_;
    RecorderStatus st;
    st.state = state_.load();
    st.width = m.outW; st.height = m.outH; st.fps = m.fps;
    st.framesWritten = m.written.load();
    st.framesDropped = m.dropRing.load() + m.dropGpu.load() + m.dropPool.load();
    const int64_t t0 = m.t0.load();
    st.elapsedSec = (t0 != INT64_MIN) ? (double)(m.lastWrittenTs.load()) / 1e7 : 0.0;
    st.file = m.s.path;
    std::lock_guard<std::mutex> lk(m.statusLock);
    st.message = m.message;
    return st;
}

// ------------------------------------------------------------------------------------------------------------------

void Recorder::Impl::FreeSlot(UINT slot, uint64_t freeValue, bool updateValue) {
    std::lock_guard<std::mutex> lk(qm);
    if (slot < slotBusy.size()) {
        slotBusy[slot] = false;
        if (updateValue) slotFreeValue[slot] = freeValue;
    }
}

void Recorder::Impl::Run() {
    {
        // low-space guard (src/disk.h): never start a recording on an almost full drive
        unsigned long long freeB = 0;
        if (disk::FreeBytes(s.path, freeB) && freeB < disk::MinFreeToStart()) {
            const std::string d = elog::Narrow(disk::DriveName(s.path));
            const std::string msg = "not started: " + (d.empty() ? std::string("the output drive") : d) + " has only " +
                                    elog::Narrow(disk::SizeText(freeB)) + " free (needs " +
                                    elog::Narrow(disk::SizeText(disk::MinFreeToStart())) + "; free space or pick another output folder)";
            LOGE("recorder: %s -- %s", msg.c_str(), elog::Narrow(s.path).c_str());
            SetMessage(msg);
            owner->state_ = RecState::Failed;
            return;
        }
    }
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    const std::string file = elog::Narrow(s.path);
    bool ok = SUCCEEDED(mfHr) && Init();
    if (!ok) {
        LOGE("recorder: start FAILED for %s (%s) -- see the lines above", file.c_str(), message.c_str());
        Cleanup();
        DeleteFileW(s.path.c_str());
        DeleteFileW(SidecarPath().c_str());   // never a sidecar without its MP4
        if (SUCCEEDED(mfHr)) MFShutdown();
        if (SUCCEEDED(coHr)) CoUninitialize();
        owner->state_ = RecState::Failed;
        return;
    }
    RecState e = RecState::Starting;
    owner->state_.compare_exchange_strong(e, RecState::Recording);
    LOGI("recorder: RECORDING -> %s", file.c_str());

    // main loop
    std::string lowDisk;              // set when the recording was stopped because the drive is almost full
    ULONGLONG lastDiskCheck = GetTickCount64();
    for (;;) {
        Item it{};
        bool idle = false;
        {
            std::unique_lock<std::mutex> lk(qm);
            auto ready = [&] { return !queue.empty() || stopReq.load(); };
            // constant_frame_rate: wake every 20 ms without a frame to write the slots of a stall as time passes (CfrIdleFill)
            if (cfr) idle = !qcv.wait_for(lk, std::chrono::milliseconds(20), ready);
            else qcv.wait(lk, ready);
            if (!idle) {
                if (queue.empty()) break;
                it = queue.front();
                queue.pop_front();
            }
        }
        if (idle) CfrIdleFill();
        else Process(it);
        // low-space guard: every 2 s; below MinFreeToContinue the recording stops like a hotkey stop (finalized, saved)
        const ULONGLONG nowTick = GetTickCount64();
        if (lowDisk.empty() && nowTick - lastDiskCheck >= 2000) {
            lastDiskCheck = nowTick;
            unsigned long long freeB = 0;
            if (disk::FreeBytes(s.path, freeB) && freeB < disk::MinFreeToContinue()) {
                const std::string d = elog::Narrow(disk::DriveName(s.path));
                lowDisk = "stopped: " + (d.empty() ? std::string("drive") : d) + " almost full (" + elog::Narrow(disk::SizeText(freeB)) + " free)";
                LOGW("recorder: %s -- stopping the recording (threshold %s)", lowDisk.c_str(),
                     elog::Narrow(disk::SizeText(disk::MinFreeToContinue())).c_str());
                owner->Stop();
            }
        }
    }
    // stop: let the game thread leave SubmitFrame, then drain what it queued
    owner->state_ = RecState::Stopping;
    {
        const ULONGLONG t = GetTickCount64();
        while (gameBusy.load() > 0 && GetTickCount64() - t < 2000) Sleep(1);
    }
    for (;;) {
        Item it{};
        {
            std::lock_guard<std::mutex> lk(qm);
            if (queue.empty()) break;
            it = queue.front();
            queue.pop_front();
        }
        Process(it);
    }
    if (hasAudio) audio.Stop();

    HRESULT fhr = S_OK;
    {
        std::lock_guard<std::mutex> lk(wm);
        if (writer) fhr = writer->Finalize();
        writer.Reset();
    }
    if (pool) pool->AllFree(5000);
    Cleanup();
    MFShutdown();
    if (SUCCEEDED(coHr)) CoUninitialize();

    const double dur = (double)lastWrittenTs.load() / 1e7;
    const uint64_t frames = written.load();
    std::string st3dReport = "st3d: skipped";
    bool fileOk = SUCCEEDED(fhr) && frames > 0;
    if (fileOk && dome.valid) {
        // VR180: a quarter of the sphere cropped on each side (+-90 degrees covered); VR360: the full sphere.
        const bool v180 = s.dome.projection == DomeProjection::Vr180;
        const uint32_t bounds[4] = {0, 0, v180 ? 0x40000000u : 0u, v180 ? 0x40000000u : 0u};
        PatchStereoSpherical(s.path, v180 ? 2 /*left-right*/ : 1 /*top-bottom*/, true, bounds, st3dReport);
        WriteSidecar(file);
    } else if (fileOk) {
        PatchSt3d(s.path, yt && ytTb ? 1 /*top-bottom*/ : 2 /*left-right*/, st3dReport);
        if (yt) WriteSidecar(file);   // record_for_youtube: tells the tray the file is already in the YouTube layout
    } else {
        DeleteFileW(s.path.c_str());
        DeleteFileW(SidecarPath().c_str());
    }
    char layoutText[96];
    if (dome.valid) snprintf(layoutText, sizeof(layoutText), "%s dome", DomeProjectionName(s.dome.projection));
    else if (yt) snprintf(layoutText, sizeof(layoutText), "YouTube %s (eye_crop %s, st3d %s)", YtPackingKey(s.layout3d),
                          YtCropKey(s.eyeCrop), ytTb ? "top-bottom" : "left-right");
    else snprintf(layoutText, sizeof(layoutText), "SBS");
    char rateText[160];
    if (cfr)
        snprintf(rateText, sizeof(rateText), "constant frame rate %u fps, %llu repeated, %llu dropped%s", fps,
                 (unsigned long long)cfrRepeated.load(), (unsigned long long)cfrDropped.load(),
                 cfrCapped ? " (a gap over 5 s was only partly filled)" : "");
    else snprintf(rateText, sizeof(rateText), "variable frame rate (real timestamps)");
    char sum[1000];
    snprintf(sum, sizeof(sum),
             "recording summary: %s | %s | %ux%u %s %s @%u fps nominal | layout %s | frames written %llu, dropped %llu "
             "(ring full %llu, gpu busy %llu, encoder stall %llu), duplicates %llu, over max_fps %llu | %s | duration %.2f s | "
             "audio %s (%llu frames = %.2f s) | finalize %s | %s",
             fileOk ? "OK" : "FAILED", file.c_str(), outW, outH, VideoCodecName(codec), rcText.c_str(), fps, layoutText,
             (unsigned long long)frames,
             (unsigned long long)(dropRing.load() + dropGpu.load() + dropPool.load()),
             (unsigned long long)dropRing.load(), (unsigned long long)dropGpu.load(), (unsigned long long)dropPool.load(),
             (unsigned long long)dropDup.load(), (unsigned long long)decimated.load(), rateText, dur,
             hasAudio ? "AAC" : "none", (unsigned long long)audioFrames.load(), audioFrames.load() / 48000.0,
             HrText(fhr).c_str(), st3dReport.c_str());
    if (fileOk) LOGI("%s", sum); else LOGE("%s", sum);
    if (ovLayersIn.load())
        LOGI("overlay summary: %llu frames with overlays composited (%llu layer x eye draws) | layers handed over %llu "
             "(%llu behind the projection layer): before their atlas existed %llu, atlas full %llu, atlas setup failed %llu "
             "| atlases: 0x%x%s",
             (unsigned long long)ovFrames, (unsigned long long)ovDraws, (unsigned long long)ovLayersIn.load(),
             (unsigned long long)ovBehind.load(), (unsigned long long)ovNotReady.load(), (unsigned long long)ovFull.load(),
             (unsigned long long)ovSetupFailed.load(), (unsigned)ovReadyMask.load(),
             ovFailedMask.load() ? " | an atlas / compositor setup FAILED" : "");
    if (stage && dome.valid) {
        LOGI("re-projection summary (dome %s, fill %s): %llu frames | dome: %llu frames painted, %llu frames without pose | "
             "D yaw %+.2f deg%s | re-projection %ux%u: GPU %.3f ms avg, %.3f ms max (%llu samples), worker CPU %.3f ms avg, "
             "%.3f ms max",
             DomeProjectionName(s.dome.projection), domeLastSeen ? "last_seen" : "black", (unsigned long long)smFrames,
             (unsigned long long)domePainted, (unsigned long long)domeNoPose, domeFrame.yawDeg,
             domeHaveFrame ? "" : " (never set: no usable pose)", vpW, vpH, gpuN ? gpuSum / gpuN : 0.0, gpuMax,
             (unsigned long long)gpuN, smFrames ? cpuSum / smFrames : 0.0, cpuMax);
    } else if (stage) {
        const double n = smFrames ? (double)smFrames : 1.0;
        LOGI("re-projection summary (head smoothing %s, symmetric fov %s): %llu frames | correction mean %.2f deg, max %.2f deg | edge guard limited the "
             "correction in %llu frames | source did not cover the output (black edges) in %llu frames | follow / "
             "catch-up active in %llu frames | no usable pose in %llu frames | filter resets %llu | re-projection %ux%u: "
             "GPU %.3f ms avg, %.3f ms max (%llu samples), worker CPU %.3f ms avg, %.3f ms max",
             s.head.Enabled() ? "on" : "off", sym.valid ? "on" : "off", (unsigned long long)smFrames, smCorrSum / n, smCorrMax, (unsigned long long)smGuarded,
             (unsigned long long)smUncovered, (unsigned long long)smCatch, (unsigned long long)smNoPose,
             (unsigned long long)smResets, vpW, vpH, gpuN ? gpuSum / gpuN : 0.0, gpuMax, (unsigned long long)gpuN,
             cpuSum / n, cpuMax);
    }
    // "saved <file>" is what the tray looks for; a low-space stop appends its reason (the tray reads the file name from `file`)
    SetMessage(fileOk ? std::string("saved ") + file + (lowDisk.empty() ? std::string() : " -- " + lowDisk)
                      : std::string("recording failed (no frames or finalize error)") + (lowDisk.empty() ? std::string() : " -- " + lowDisk));
    owner->state_ = RecState::Idle;
}

bool Recorder::Impl::Init() {
    // --- encoder device on the game's adapter ---
    const LUID luid = src->AdapterLuid();
    if (!enc || encLuid.LowPart != luid.LowPart || encLuid.HighPart != luid.HighPart) {
        std::string name;
        enc = CreateEncoderDevice(luid, &name);
        if (!enc) { SetMessage("no encoder device"); return false; }
        encLuid = luid;
        encCtx.Reset(); encCtx4.Reset(); devMgr.Reset();
        enc->GetImmediateContext(&encCtx);
        encCtx.As(&encCtx4);
    }
    if (!encCtx4) { SetMessage("no ID3D11DeviceContext4"); return false; }

    // --- output size (from the re-projection output when symmetric_fov changes it) ---
    if (!FitOutputSize()) return false;
    bitrate = s.bitrate ? s.bitrate : VideoCodecDefaultMbps(codec) * 1000000u;

    char qpNote[48] = "";
    if (s.rateControl == RateControl::Qp) snprintf(qpNote, sizeof(qpNote), " (QP %u; the bitrate is not used)", s.qp);
    LOGI("recorder: init %s -- SBS %ux%u (left %u) %s %s -> %ux%u %s %.0f Mb/s, %u fps nominal, GOP %u, rate_control %s%s",
         GfxApiName(src->Api()), W, H, leftW, DxgiFormatName(fmt), srgb ? "sRGB-encoded" : "linear", outW, outH,
         VideoCodecName(codec), bitrate / 1e6, fps, fps * 2, RateControlKey(s.rateControl), qpNote);
    if (cfr)
        LOGI("recorder: constant frame rate %u fps: one frame every %.3f ms at k / %u s with constant durations (a frame in an "
             "already written slot is dropped, a missing slot repeats the previous frame, at most 5 s per gap); audio keeps its real "
             "timestamps", fps, 1000.0 / fps, fps);

    if (dome.valid) {
        const char* pn = DomeProjectionName(s.dome.projection);
        char why[200];
        if (dome.fromExplicit) snprintf(why, sizeof(why), "dome_width %u", s.dome.width);
        else if (dome.fallback) snprintf(why, sizeof(why), "no usable view fov at record start, assumed +-45 deg eye images");
        else snprintf(why, sizeof(why), "source density %.1f px/rad", dome.density);
        LOGI("recorder: dome %s: %s -> %u px per eye%s; per eye %ux%u, frame %ux%u (%s)", pn, why, dome.wanted,
             dome.clamped ? (std::string(", clamped to ") + std::to_string(dome.eyeW) + " (frame <= 8192)").c_str() : "",
             dome.eyeW, dome.eyeH, dome.frameW, dome.frameH,
             s.dome.projection == DomeProjection::Vr180 ? "left | right, lon +-90 x lat +-90 each"
                                                        : "left on top / right below, lon +-180 x lat +-90 each");
        LOGI("recorder: dome %s: fill %s, fill_fade_s %.1f%s, window_feather_deg %.1f, dome_forward start (world-locked to the "
             "projection space, yaw of the first frame with a pose) -- symmetric_fov, head_smoothing, head_follow_deg, roll_lock, "
             "output_fov_scale and edge_guard are ignored in dome mode",
             pn, s.dome.fill == DomeFill::LastSeen ? "last_seen" : "black", s.dome.fadeS, s.dome.fadeS > 0 ? "" : " (never fade)",
             s.dome.featherDeg);
    } else if (sym.valid) {
        const double D = 180.0 / 3.14159265358979323846;
        const float* fl = s.startFov[0];
        const float* fr = s.startFov[1];
        LOGI("recorder: symmetric fov: source L[%+.1f,%+.1f]x[%+.1f,%+.1f] R[%+.1f,%+.1f]x[%+.1f,%+.1f] -> output "
             "\xC2\xB1%.1f\xC2\xB0 x \xC2\xB1%.1f\xC2\xB0 per eye%s, %ux%u per eye at the source density (SBS %ux%u, from %ux%u)",
             fl[0] * D, fl[1] * D, fl[3] * D, fl[2] * D, fr[0] * D, fr[1] * D, fr[3] * D, fr[2] * D,
             std::atan(sym.tanH) * D, std::atan(sym.tanV) * D,
             s.head.Enabled() ? " (x output_fov_scale while smoothing)" : "", sym.eyeW, sym.eyeH, rpNativeW, rpNativeH, W, H);
    } else if (s.head.symmetricFov) {
        LOGW("recorder: symmetric fov: no usable view fov at record start -- recording the raw eye images");
    } else {
        LOGI("recorder: symmetric fov off (symmetric_fov = 0): raw eye images");
    }
    if (ytWarn == 1)
        LOGW("recorder: record_for_youtube ignored: projection %s records its own dome layout", DomeProjectionName(s.dome.projection));
    else if (ytWarn == 2)
        LOGW("recorder: record_for_youtube: no usable view fov at record start -- recording the normal SBS layout instead "
             "(no YouTube layout, no sidecar; the tray converts it as usual)");
    if (yt) {
        const double D = 180.0 / 3.14159265358979323846;
        const YtLayout& yl = ytFit.native;
        if (ytForcedSym)
            LOGI("recorder: record_for_youtube: symmetric_fov = 0 overridden -- the YouTube layout needs the symmetric frustum "
                 "(on for this recording)");
        if (ytShortEnv)
            LOGI("recorder: XR_EYE_CAPTURE_TEST_YT_SHORT = %d: YouTube frame short side %d instead of %d (test override)",
                 ytShort, ytShort, kYtShortSide);
        char lim[160] = "";
        if (ytFit.Scaled()) {
            char why[96];
            if (ytFit.byWidth && ytFit.byCodec)
                snprintf(why, sizeof(why), "max_width %u, %s limit %u", s.maxWidth, VideoCodecName(s.codec), VideoCodecMaxSide(s.codec));
            else if (ytFit.byWidth) snprintf(why, sizeof(why), "max_width %u", s.maxWidth);
            else snprintf(why, sizeof(why), "%s limit %u", VideoCodecName(s.codec), VideoCodecMaxSide(s.codec));
            snprintf(lim, sizeof(lim), " -> scaled to %dx%d (each eye %dx%d; %s)", ytFit.fw, ytFit.fh, ytFit.sw, ytFit.sh, why);
        }
        char pad[160] = "";
        if (yl.pw > yl.cw)   // 4:3pad: the bars, in eye units and in the frame
            snprintf(pad, sizeof(pad), " -> black bars left and right to %dx%d (picture %d wide at x %d of each eye)", yl.pw, yl.ch,
                     yl.vw, yl.vx);
        LOGI("recorder: YouTube layout: eye %ux%u -> crop %s %dx%d%s -> %s frame %dx%d (each eye %dx%d; short side %d, long side "
             "<= 8192)%s, st3d %s -- output \xC2\xB1%.1f\xC2\xB0 x \xC2\xB1%.1f\xC2\xB0 per eye%s",
             sym.eyeW, sym.eyeH, YtCropKey(s.eyeCrop), yl.cw, yl.ch, pad, YtPackingKey(s.layout3d), yl.fw, yl.fh, yl.sw, yl.sh,
             ytShort, lim, ytTb ? "top-bottom" : "left-right", std::atan(sym.tanH) * D, std::atan(ytTanV) * D,
             s.head.Enabled() ? " (x output_fov_scale while smoothing)" : "");
    }
    if (stage && yt) {
        const YtLayout& yl = ytFit.native;
        char pad[96] = "";
        if (ytFit.vw < ytFit.sw)   // 4:3pad
            snprintf(pad, sizeof(pad), "; picture %dx%d at x +%d in each, black bars left and right", ytFit.vw, ytFit.sh, ytFit.vx);
        LOGI("recorder: re-projection rendered at %ux%u (YouTube %s: left eye at 0,0 | right eye at %u,%u, %dx%d each%s; from %dx%d "
             "per eye, %.2fx wide, %.2fx tall; %ux%u bilinear taps per output pixel) -- the video processor does not scale",
             vpW, vpH, YtPackingKey(s.layout3d), outRightX, outBottomY, ytFit.sw, ytFit.sh, pad, yl.cw, yl.ch,
             (double)ytFit.vw / std::max(1, yl.cw), (double)ytFit.sh / std::max(1, yl.ch), rpTaps, rpTaps);
    } else if (stage && !dome.valid) {
        if (rpScaled) {
            char why[96];
            if (s.maxWidth && rpNativeW > s.maxWidth) snprintf(why, sizeof(why), "max_width %u", s.maxWidth);
            else snprintf(why, sizeof(why), "max_width %u, %s limit %u", s.maxWidth, VideoCodecName(codec), VideoCodecMaxSide(codec));
            LOGI("recorder: re-projection rendered at %ux%u (left %u | right %u; %s; native %ux%u, %.2fx smaller per axis, "
                 "%ux%u bilinear taps per output pixel) -- the video processor does not scale",
                 vpW, vpH, rpEyeW[0], rpEyeW[1], why, rpNativeW, rpNativeH, (double)rpNativeW / std::max<UINT>(1, vpW),
                 rpTaps, rpTaps);
        } else {
            LOGI("recorder: re-projection rendered at %ux%u (native size; max_width %u)", vpW, vpH, s.maxWidth);
        }
    }
    if (dome.valid) {
        // smoothing is not used in dome mode (logged above)
    } else if (s.head.Enabled())
        LOGI("recorder: head smoothing ON -- head_smoothing %.2f (time constant %.3f s), head_follow_deg %.1f, roll_lock %d, "
             "output_fov_scale %.2f, edge_guard %d",
             s.head.smoothing, HeadSmoothingTimeConstant(s.head.smoothing), s.head.followDeg, s.head.rollLock ? 1 : 0,
             s.head.fovScale, s.head.edgeGuard ? 1 : 0);
    else
        LOGI("recorder: head smoothing off (head_smoothing = 0)");

    // --- shared ring ---
    if (!src->CreateRing(enc.Get(), W, H, fmt, kRingSize, ring)) { SetMessage("shared ring creation failed"); return false; }
    ringCreated = true;
    {
        std::lock_guard<std::mutex> lk(qm);
        slotBusy.assign(kRingSize, false);
        slotFreeValue.assign(kRingSize, 0);
    }
    freeCounter = 0;

    if (!InitVideoProcessor()) return false;

    // --- audio (before the writer: the stream must exist before BeginWriting) ---
    // Capture starts first (OnAudio drops everything until the writer exists and video has started); if it cannot
    // start, the file gets no audio stream at all rather than an empty one that would stall the muxer.
    hasAudio = false;
    if (s.audio) {
        aacBytes = ProbeAacBytesPerSecond();
        if (!aacBytes) LOGW("audio: no AAC encoder found -- recording without audio");
        else if (!audio.Start(s.audioDevice, [this](const int16_t* p, uint32_t n, int64_t t) { OnAudio(p, n, t); }))
            LOGW("audio: loopback capture failed to start -- recording without audio");
        else hasAudio = true;
    }

    // --- sink writer (attempts) ---
    if (!devMgr) {
        HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &devMgr);
        if (SUCCEEDED(hr)) hr = devMgr->ResetDevice(enc.Get(), resetToken);
        if (FAILED(hr)) { LOGE("recorder: IMFDXGIDeviceManager failed hr=0x%08lx", (unsigned long)hr); SetMessage("DXGI device manager failed"); return false; }
    }
    // Attempts in order. AV1 is hardware only (no software AV1 MFT is fast enough for VR frames): AV1 hw, then the HEVC
    // list. HEVC: hw, H.264 hw (only <= 4096), HEVC sw, H.264 sw (only <= 4096). H.264: hw, sw. HEVC and AV1 both take
    // 8192, so a fallback from AV1 keeps the size fitted above. XR_EYE_CAPTURE_TEST_NO_AV1 (tests only) makes the AV1
    // attempt fail in CreateWriter (after AddStream), as if the GPU had no AV1 encoder.
    struct Attempt { VideoCodec c; bool hw; };
    const VideoCodec want = codec;
    const bool h264Fits = outW <= 4096 && outH <= 4096;
    std::vector<Attempt> attempts;
    if (want == VideoCodec::Av1) attempts.push_back({VideoCodec::Av1, true});
    if (want == VideoCodec::H264) {
        attempts.push_back({VideoCodec::H264, true});
        attempts.push_back({VideoCodec::H264, false});
    } else {
        attempts.push_back({VideoCodec::Hevc, true});
        if (h264Fits) attempts.push_back({VideoCodec::H264, true});
        attempts.push_back({VideoCodec::Hevc, false});
        if (h264Fits) attempts.push_back({VideoCodec::H264, false});
    }
    HRESULT hr = E_FAIL;
    for (const Attempt& a : attempts) {
        std::string why;
        bitrate = s.bitrate ? s.bitrate : VideoCodecDefaultMbps(a.c) * 1000000u;   // the default follows the codec tried
        hr = CreateWriter(a.c, a.hw, why);
        if (SUCCEEDED(hr)) {
            if (a.c != want)
                LOGW("recorder: %s unavailable -- fell back to %s (%s transforms, %.0f Mb/s)", VideoCodecName(want),
                     VideoCodecName(a.c), a.hw ? "hardware" : "software", bitrate / 1e6);
            codec = a.c;
            break;
        }
        LOGW("recorder: sink writer attempt %s %s failed at %s hr=0x%08lx", VideoCodecName(a.c),
             a.hw ? "hardware" : "software", why.c_str(), (unsigned long)hr);
        if (a.c == VideoCodec::Av1)
            LOGW("recorder: no usable hardware AV1 encoder (needs AV1 encoding on the GPU, e.g. NVIDIA RTX 40 / 50) -- trying HEVC");
        DeleteFileW(s.path.c_str());
    }
    if (FAILED(hr)) { SetMessage("no usable video encoder"); return false; }
    LogTransforms();
    {   // what the encoder MFT reports back (ICodecAPI::GetValue; "n/a" = it does not say)
        ComPtr<ICodecAPI> api;
        {
            std::lock_guard<std::mutex> lk(wm);
            api = EncoderCodecApi(writer.Get(), vStream);
        }
        if (api)
            LOGI("recorder: encoder reports: rate control mode %s (0 CBR, 2 unconstrained VBR, 3 quality), mean bitrate %s, "
                 "QP %s, quality vs speed %s", CodecApiGetText(api.Get(), CODECAPI_AVEncCommonRateControlMode).c_str(),
                 CodecApiGetText(api.Get(), CODECAPI_AVEncCommonMeanBitRate).c_str(),
                 CodecApiGetText(api.Get(), CODECAPI_AVEncVideoEncodeQP).c_str(),
                 CodecApiGetText(api.Get(), CODECAPI_AVEncCommonQualityVsSpeed).c_str());
    }
    if (FAILED(devMgr->OpenDeviceHandle(&devHandle))) devHandle = nullptr;
    if (stopReq.load()) LOGW("recorder: stop requested during start");
    return true;
}

// Encoded size outW x outH from the video processor input vpW x vpH: max_width, then the codec limit, even.
bool Recorder::Impl::FitOutputSize() {
    outW = vpW; outH = vpH;
    auto scaleTo = [&](UINT maxW, UINT maxH) {
        if (outW > maxW) { outH = (UINT)((uint64_t)outH * maxW / outW); outW = maxW; }
        if (outH > maxH) { outW = (UINT)((uint64_t)outW * maxH / outH); outH = maxH; }
    };
    if (s.maxWidth) scaleTo(s.maxWidth, 16384);
    const UINT codecMax = VideoCodecMaxSide(codec);
    if (outW > codecMax || outH > codecMax) {
        LOGW("recorder: %ux%u exceeds the %s limit of %u -- scaling down (set max_width or use codec = hevc)", outW, outH,
             VideoCodecName(codec), codecMax);
        scaleTo(codecMax, codecMax);
    }
    outW &= ~1u; outH &= ~1u;
    if (outW < 64 || outH < 64) { SetMessage("frame too small"); return false; }
    return true;
}

bool Recorder::Impl::InitVideoProcessor() {
    HRESULT hr = enc.As(&vdev);
    if (SUCCEEDED(hr)) hr = encCtx.As(&vctx);
    if (FAILED(hr)) { LOGE("recorder: encoder device has no video interfaces hr=0x%08lx", (unsigned long)hr); SetMessage("no video device"); return false; }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {fps, 1};
    cd.InputWidth = vpW; cd.InputHeight = vpH;
    cd.OutputFrameRate = {fps, 1};
    cd.OutputWidth = outW; cd.OutputHeight = outH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    hr = vdev->CreateVideoProcessorEnumerator(&cd, &venum);
    if (FAILED(hr)) { LOGE("recorder: CreateVideoProcessorEnumerator hr=0x%08lx", (unsigned long)hr); SetMessage("video processor"); return false; }
    const DXGI_FORMAT vpFmt = srgb ? fmt : DXGI_FORMAT_R8G8B8A8_UNORM;   // linear input goes through the pre-pass
    UINT inSup = 0, outSup = 0;
    venum->CheckVideoProcessorFormat(vpFmt, &inSup);
    venum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outSup);
    if (!(inSup & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(outSup & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        LOGE("recorder: video processor cannot convert %s -> NV12 (in 0x%x out 0x%x)", DxgiFormatName(vpFmt), inSup, outSup);
        SetMessage("format not supported by the video processor");
        return false;
    }
    hr = vdev->CreateVideoProcessor(venum.Get(), 0, &vp);
    if (FAILED(hr)) { LOGE("recorder: CreateVideoProcessor hr=0x%08lx", (unsigned long)hr); SetMessage("video processor"); return false; }

    // Colour: input = full-range sRGB-encoded RGB (linear formats are encoded by the pre-pass), output = BT.709 limited.
    const DXGI_COLOR_SPACE_TYPE inCs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const DXGI_COLOR_SPACE_TYPE outCs = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    ComPtr<ID3D11VideoContext1> vctx1;
    ComPtr<ID3D11VideoProcessorEnumerator1> venum1;
    bool cs1 = SUCCEEDED(vctx.As(&vctx1));
    if (cs1 && SUCCEEDED(venum.As(&venum1))) {
        BOOL sup = FALSE;
        if (SUCCEEDED(venum1->CheckVideoProcessorFormatConversion(vpFmt, inCs, DXGI_FORMAT_NV12, outCs, &sup)) && !sup)
            LOGW("recorder: the video processor reports no %s RGB -> NV12 BT.709 conversion; colours may be off", DxgiFormatName(vpFmt));
    }
    if (cs1) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, inCs);
        vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), outCs);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.RGB_Range = 0;          // full
        in.YCbCr_Matrix = 1;       // 709
        out.RGB_Range = 0;
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in);
        vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &out);
    }
    const RECT srcR{0, 0, (LONG)vpW, (LONG)vpH}, dstR{0, 0, (LONG)outW, (LONG)outH};
    vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &srcR);
    vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dstR);
    vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dstR);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    vctx->VideoProcessorSetStreamAlpha(vp.Get(), 0, FALSE, 1.0f);
    D3D11_VIDEO_COLOR bg{};
    bg.RGBA.A = 1.0f;
    vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), FALSE, &bg);
    vctx->VideoProcessorSetOutputAlphaFillMode(vp.Get(), D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);

    // input views (directly on the shared ring; local copies if the driver refuses)
    inViews.clear();
    localCopies.clear();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (!srgb) {
        if (!InitLinearPass()) return false;
        hr = vdev->CreateVideoProcessorInputView(linTex.Get(), venum.Get(), &iv, &linView);
        if (FAILED(hr)) { LOGE("recorder: input view on the linear pre-pass texture hr=0x%08lx", (unsigned long)hr); SetMessage("video processor input view"); return false; }
    }
    if (stage && !InitReprojection(vpFmt)) {
        // raw layout (no dome / symmetric fov) rendered at the recorded size: the plain path can still record
        const bool rawScaled = !dome.valid && !sym.valid && rpScaled;
        if (dome.valid || yt || ((vpW != W || vpH != H) && !rawScaled)) { SetMessage("re-projection setup failed"); return false; }   // VP sized for its output
        LOGW("recorder: head smoothing disabled for this recording (re-projection setup failed)");
        stage = false;
        if (rawScaled) {   // the VP was set up for the scaled stage output: back to W x H in, scaled by the VP as before
            rpView.Reset(); rpRtv.Reset(); rpTex.Reset();
            vpW = W; vpH = H; outRightX = leftW;
            rpScaled = false; rpTaps = 1;
            rpEyeW[0] = rpNativeEyeW[0]; rpEyeW[1] = rpNativeEyeW[1];
            if (!FitOutputSize()) return false;
            LOGW("recorder: plain recording instead: %ux%u -> %ux%u, scaled by the video processor", W, H, outW, outH);
            return InitVideoProcessor();   // stage is off now, so no second re-projection attempt
        }
    }
    const bool vpFromRing = srgb && !stage;   // otherwise the VP reads the pre-pass / re-projection output
    bool direct = true;
    for (auto& t : ring.tex) {
        if (!vpFromRing) break;
        ComPtr<ID3D11VideoProcessorInputView> v;
        if (FAILED(vdev->CreateVideoProcessorInputView(t.Get(), venum.Get(), &iv, &v))) { direct = false; break; }
        inViews.push_back(v);
    }
    if (vpFromRing && !direct) {
        LOGW("recorder: video processor cannot read the shared ring directly -- adding one GPU copy per frame");
        inViews.clear();
        for (size_t i = 0; i < ring.tex.size(); ++i) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> t;
            ComPtr<ID3D11VideoProcessorInputView> v;
            if (FAILED(enc->CreateTexture2D(&d, nullptr, &t)) ||
                FAILED(hr = vdev->CreateVideoProcessorInputView(t.Get(), venum.Get(), &iv, &v))) {
                LOGE("recorder: CreateVideoProcessorInputView hr=0x%08lx", (unsigned long)hr);
                SetMessage("video processor input view");
                return false;
            }
            localCopies.push_back(t);
            inViews.push_back(v);
        }
    }

    // NV12 pool
    nv12.clear(); outViews.clear(); poolCb.clear();
    pool = std::make_shared<Nv12Pool>();
    pool->busy.assign(kNv12Pool, false);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < kNv12Pool; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = outW; d.Height = outH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        ComPtr<ID3D11Texture2D> t;
        hr = enc->CreateTexture2D(&d, nullptr, &t);
        if (FAILED(hr)) {
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = enc->CreateTexture2D(&d, nullptr, &t);
        }
        ComPtr<ID3D11VideoProcessorOutputView> v;
        if (SUCCEEDED(hr)) hr = vdev->CreateVideoProcessorOutputView(t.Get(), venum.Get(), &ov, &v);
        if (FAILED(hr)) { LOGE("recorder: NV12 surface %ux%u failed hr=0x%08lx", outW, outH, (unsigned long)hr); SetMessage("NV12 surface"); return false; }
        nv12.push_back(t);
        outViews.push_back(v);
        ComPtr<IMFAsyncCallback> cb;
        cb.Attach(new PoolReturn(pool, (int)i));
        poolCb.push_back(cb);
    }
    return true;
}

bool Recorder::Impl::InitLinearPass() {
    ComPtr<ID3DBlob> code, errs;
    HRESULT hr = D3DCompile(kLinearToSrgbCs, sizeof(kLinearToSrgbCs) - 1, "linear_to_srgb", nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
    if (FAILED(hr)) {
        LOGE("recorder: linear->sRGB shader compile failed hr=0x%08lx %s", (unsigned long)hr,
             errs ? (const char*)errs->GetBufferPointer() : "");
        SetMessage("shader compile");
        return false;
    }
    hr = enc->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &linCs);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = enc->CreateTexture2D(&d, nullptr, &linTex);
    if (SUCCEEDED(hr)) hr = enc->CreateUnorderedAccessView(linTex.Get(), nullptr, &linUav);
    linSrv.clear();
    for (size_t i = 0; SUCCEEDED(hr) && i < ring.tex.size(); ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> v;
        hr = enc->CreateShaderResourceView(ring.tex[i].Get(), &sd, &v);
        linSrv.push_back(v);
    }
    if (FAILED(hr)) { LOGE("recorder: linear->sRGB pre-pass setup failed hr=0x%08lx", (unsigned long)hr); SetMessage("linear pre-pass"); return false; }
    LOGI("recorder: linear swapchain format -> sRGB-encoding pre-pass (compute) on the encoder device");
    return true;
}

bool Recorder::Impl::InitReprojection(DXGI_FORMAT vpFmt) {
    ComPtr<ID3DBlob> vsCode, psCode, errs;
    HRESULT hr = D3DCompile(kReprojVs, sizeof(kReprojVs) - 1, "reproject_vs", nullptr, nullptr, "main", "vs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, &errs);
    if (SUCCEEDED(hr) && dome.valid) {
        errs.Reset();
        const D3D_SHADER_MACRO lastSeen[] = {{"DOME_LAST_SEEN", "1"}, {nullptr, nullptr}};
        hr = D3DCompile(kDomePs, sizeof(kDomePs) - 1, "dome_ps", domeLastSeen ? lastSeen : nullptr, nullptr, "main", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, &errs);
    } else if (SUCCEEDED(hr)) {
        errs.Reset();
        hr = D3DCompile(kReprojPs, sizeof(kReprojPs) - 1, "reproject_ps", nullptr, nullptr, "main", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, &errs);
    }
    if (FAILED(hr)) {
        LOGE("recorder: re-projection shader compile failed hr=0x%08lx %s", (unsigned long)hr,
             errs ? (const char*)errs->GetBufferPointer() : "");
        return false;
    }
    hr = enc->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &rpVs);
    if (SUCCEEDED(hr)) hr = enc->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &rpPs);
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = dome.valid ? sizeof(DomeConstants) : sizeof(RpConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = enc->CreateBuffer(&bd, nullptr, &rpCb);
    }
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = enc->CreateSamplerState(&sd, &rpSmp);
    }
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = vpW; d.Height = vpH; d.MipLevels = 1; d.ArraySize = 1; d.Format = vpFmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = enc->CreateTexture2D(&d, nullptr, &rpTex);
    }
    if (SUCCEEDED(hr)) hr = enc->CreateRenderTargetView(rpTex.Get(), nullptr, &rpRtv);
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        hr = vdev->CreateVideoProcessorInputView(rpTex.Get(), venum.Get(), &iv, &rpView);
    }
    rpRingSrv.clear();
    rpLinSrv.Reset();
    if (SUCCEEDED(hr)) {
        if (srgb) {
            for (size_t i = 0; SUCCEEDED(hr) && i < ring.tex.size(); ++i) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = fmt;   // typed UNORM / float member of the copy family: no sRGB decode on sampling
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                ComPtr<ID3D11ShaderResourceView> v;
                hr = enc->CreateShaderResourceView(ring.tex[i].Get(), &sd, &v);
                rpRingSrv.push_back(v);
            }
        } else {
            hr = enc->CreateShaderResourceView(linTex.Get(), nullptr, &rpLinSrv);
        }
    }
    if (FAILED(hr)) { LOGE("recorder: re-projection setup failed hr=0x%08lx", (unsigned long)hr); return false; }
    rpQ.clear();
    rpQ.resize(kTimingQueries);
    rpQi = 0;
    for (auto& q : rpQ) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(enc->CreateQuery(&qd, &q.disjoint)) || FAILED(enc->CreateQuery(&qt, &q.begin)) ||
            FAILED(enc->CreateQuery(&qt, &q.end))) {
            rpQ.clear();   // timing is optional
            break;
        }
    }
    const double outMb = (double)vpW * vpH * (vpFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4) / (1024.0 * 1024.0);
    if (dome.valid) {
        double domeMb = 0;
        if (domeLastSeen && !InitDomeTextures(vpFmt, domeMb)) return false;
        LOGI("recorder: dome pass on the encoder device: %ux%u -> %ux%u, %s -> %s, +%.0f MB VRAM (output %.0f MB%s)", W, H,
             vpW, vpH, srgb ? "ring" : "linear pre-pass output", DxgiFormatName(vpFmt), outMb + domeMb, outMb,
             domeLastSeen ? (std::string(" + last_seen dome colour x2 / time x2 ") + std::to_string((int)std::lround(domeMb)) + " MB").c_str()
                          : ", fill black: no persistent dome");
        return true;
    }
    LOGI("recorder: re-projection pass on the encoder device: %ux%u -> %ux%u, %s -> %s, +%.0f MB VRAM",
         W, H, vpW, vpH, srgb ? "ring" : "linear pre-pass output", DxgiFormatName(vpFmt), outMb);
    return true;
}

// fill = last_seen: dome colour (the stage format) and last-write time (R32_FLOAT, seconds), two of each (ping-pong).
bool Recorder::Impl::InitDomeTextures(DXGI_FORMAT vpFmt, double& mb) {
    HRESULT hr = S_OK;
    for (int i = 0; SUCCEEDED(hr) && i < 2; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = vpW; d.Height = vpH; d.MipLevels = 1; d.ArraySize = 1; d.Format = vpFmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = enc->CreateTexture2D(&d, nullptr, &domeTex[i]);
        if (SUCCEEDED(hr)) hr = enc->CreateRenderTargetView(domeTex[i].Get(), nullptr, &domeRtv[i]);
        if (SUCCEEDED(hr)) hr = enc->CreateShaderResourceView(domeTex[i].Get(), nullptr, &domeSrv[i]);
        d.Format = DXGI_FORMAT_R32_FLOAT;
        if (SUCCEEDED(hr)) hr = enc->CreateTexture2D(&d, nullptr, &domeTimeTex[i]);
        if (SUCCEEDED(hr)) hr = enc->CreateRenderTargetView(domeTimeTex[i].Get(), nullptr, &domeTimeRtv[i]);
        if (SUCCEEDED(hr)) hr = enc->CreateShaderResourceView(domeTimeTex[i].Get(), nullptr, &domeTimeSrv[i]);
    }
    if (FAILED(hr)) { LOGE("recorder: dome textures %ux%u failed hr=0x%08lx", vpW, vpH, (unsigned long)hr); return false; }
    mb = 2.0 * (double)vpW * vpH * ((vpFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4) + 4) / (1024.0 * 1024.0);
    domeCur = 0;
    domeClear = true;   // black / never painted, done at the first frame inside the device lock
    return true;
}

// Dome constants for one frame (worker thread). D is fixed at the first frame with a usable pose; frames without one
// paint nothing (the dome is shown as it is, fading).
void Recorder::Impl::DomeConstantsFor(const Item& it, DomeConstants& dc) {
    SmoothQuat q[2];
    for (int e = 0; e < 2; ++e) {
        q[e].x = it.eye[e].q[0]; q[e].y = it.eye[e].q[1]; q[e].z = it.eye[e].q[2]; q[e].w = it.eye[e].q[3];
    }
    const bool ok0 = it.eye[0].valid, ok1 = it.eye[1].valid;
    const double now = (double)(it.disp - t0Disp.load()) * 1e-9;
    if (!ok0 && !ok1) {
        ++domeNoPose;
    } else {
        if (!domeHaveFrame) {
            domeFrame = ComputeDomeFrame(ok0 ? q[0] : q[1], ok1 ? q[1] : q[0]);
            domeHaveFrame = true;
            LOGI("recorder: dome frame D fixed at the first frame with a pose (t = %.3f s): yaw %+.2f deg%s, pitch = roll = 0 "
                 "(horizon level), world-locked to the projection layer's space",
                 now, domeFrame.yawDeg, domeFrame.degenerate ? " (looking straight up / down: forward = -Z)" : "");
        }
        ++domePainted;
    }
    const bool v180 = s.dome.projection == DomeProjection::Vr180;
    for (int e = 0; e < 2; ++e) {
        const ItemEye& ie = it.eye[e];
        DomeEyeCb& de = dc.eye[e];
        if (ie.valid && domeHaveFrame) {
            double m[9];
            DomeEyeMatrix(domeFrame.D, q[e], m);
            for (int c = 0; c < 3; ++c) { de.r0[c] = (float)m[c]; de.r1[c] = (float)m[3 + c]; de.r2[c] = (float)m[6 + c]; }
            de.r0[3] = 1.0f;   // paint
            for (int i = 0; i < 4; ++i) de.srcTan[i] = (float)std::tan((double)ie.fov[i]);
        } else {
            de.r0[0] = de.r1[1] = de.r2[2] = 1.0f;   // not painted (r0.w = 0)
            de.srcTan[0] = -1.0f; de.srcTan[1] = 1.0f; de.srcTan[2] = 1.0f; de.srcTan[3] = -1.0f;
        }
        de.src[0] = (float)ie.x0; de.src[1] = 0.0f;
        de.src[2] = (float)std::max<UINT>(1, ie.w); de.src[3] = (float)std::max<UINT>(1, ie.h);
        de.dst[0] = (float)(v180 ? e * dome.eyeW : 0); de.dst[1] = (float)(v180 ? 0 : e * dome.eyeH);
        de.dst[2] = (float)dome.eyeW; de.dst[3] = (float)dome.eyeH;
    }
    dc.misc[0] = 1.0f / (float)W; dc.misc[1] = 1.0f / (float)H;
    dc.dome[0] = (float)DomeLonSpan(s.dome.projection);
    dc.dome[1] = (float)now;
    dc.dome[2] = s.dome.fadeS;
    dc.dome[3] = (float)(s.dome.featherDeg * 3.14159265358979323846 / 180.0);
}

// <name>.json next to the MP4 (s.path with its extension replaced).
std::wstring Recorder::Impl::SidecarPath() const {
    std::wstring base = s.path;
    const size_t dot = base.find_last_of(L'.'), slash = base.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) base.resize(dot);
    return base + L".json";
}

// <name>.json next to the MP4: what the tray needs to tag the upload. Dome recordings and record_for_youtube recordings
// only; no sidecar = flat SBS. One line:
//   dome:    {"projection":"vr180","layout":"left_right","eyes":2,"width":4356,"height":2178,"dome_width":2178}
//   YouTube: {"projection":"flat","layout":"top_bottom","packing":"tb_half","eye_crop":"16:9","eyes":2,"width":7680,"height":4320,"youtube":1}
void Recorder::Impl::WriteSidecar(const std::string& file) {
    const std::wstring json = SidecarPath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, json.c_str(), L"wb") != 0 || !f) {
        LOGW("recorder: could not write the sidecar %s", elog::Narrow(json).c_str());
        return;
    }
    char line[256];
    if (dome.valid) {
        const bool v180 = s.dome.projection == DomeProjection::Vr180;
        snprintf(line, sizeof(line), "{\"projection\":\"%s\",\"layout\":\"%s\",\"eyes\":2,\"width\":%u,\"height\":%u,\"dome_width\":%u}",
                 DomeProjectionName(s.dome.projection), v180 ? "left_right" : "top_bottom", outW, outH, v180 ? outW / 2 : outW);
    } else {
        snprintf(line, sizeof(line),
                 "{\"projection\":\"flat\",\"layout\":\"%s\",\"packing\":\"%s\",\"eye_crop\":\"%s\",\"eyes\":2,\"width\":%u,\"height\":%u,\"youtube\":1}",
                 ytTb ? "top_bottom" : "left_right", YtPackingKey(s.layout3d), YtCropKey(s.eyeCrop), outW, outH);
    }
    fprintf(f, "%s\n", line);
    fclose(f);
    LOGI("recorder: sidecar %s (%s, %ux%u) for %s: %s", elog::Narrow(json).c_str(),
         dome.valid ? DomeProjectionName(s.dome.projection) : "flat, YouTube layout", outW, outH, file.c_str(), line);
}

void Recorder::Impl::ReadTimings() {
    for (auto& q : rpQ) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (encCtx->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (encCtx->GetData(q.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (encCtx->GetData(q.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (dj.Disjoint || !dj.Frequency || b < a) continue;
        const double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
        gpuSum += ms;
        if (ms > gpuMax) gpuMax = ms;
        ++gpuN;
    }
}

void Recorder::Impl::Reproject(ID3D11ShaderResourceView* const* srvs, UINT nSrv, ID3D11RenderTargetView* const* rtvs, UINT nRtv,
                               const void* c, size_t cSize) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(encCtx->Map(rpCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, c, cSize);
        encCtx->Unmap(rpCb.Get(), 0);
    }
    ReadTimings();
    TsQuery* tq = nullptr;
    if (!rpQ.empty()) {
        TsQuery& q = rpQ[rpQi++ % rpQ.size()];
        if (!q.pending) tq = &q;   // still in flight: skip timing this frame rather than wait
    }
    if (tq) { encCtx->Begin(tq->disjoint.Get()); encCtx->End(tq->begin.Get()); }

    ID3D11Buffer* cb = rpCb.Get();
    ID3D11SamplerState* smp = rpSmp.Get();
    const D3D11_VIEWPORT vpt{0.0f, 0.0f, (float)vpW, (float)vpH, 0.0f, 1.0f};
    encCtx->IASetInputLayout(nullptr);
    encCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    encCtx->VSSetShader(rpVs.Get(), nullptr, 0);
    encCtx->GSSetShader(nullptr, nullptr, 0);
    encCtx->HSSetShader(nullptr, nullptr, 0);
    encCtx->DSSetShader(nullptr, nullptr, 0);
    encCtx->PSSetShader(rpPs.Get(), nullptr, 0);
    encCtx->PSSetConstantBuffers(0, 1, &cb);
    encCtx->PSSetShaderResources(0, nSrv, srvs);
    encCtx->PSSetSamplers(0, 1, &smp);
    encCtx->RSSetState(nullptr);
    encCtx->RSSetViewports(1, &vpt);
    encCtx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    encCtx->OMSetDepthStencilState(nullptr, 0);
    encCtx->OMSetRenderTargets(nRtv, rtvs, nullptr);
    encCtx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[3] = {};
    encCtx->OMSetRenderTargets(0, nullptr, nullptr);
    encCtx->PSSetShaderResources(0, nSrv, nullSrv);
    encCtx->VSSetShader(nullptr, nullptr, 0);
    encCtx->PSSetShader(nullptr, nullptr, 0);

    if (tq) { encCtx->End(tq->end.Get()); encCtx->End(tq->disjoint.Get()); tq->pending = true; }
}

HRESULT Recorder::Impl::CreateWriter(VideoCodec c, bool hardware, std::string& why) {
    std::lock_guard<std::mutex> lk(wm);
    writer.Reset();
    DeleteFileW(s.path.c_str());
    ComPtr<IMFAttributes> a;
    HRESULT hr = MFCreateAttributes(&a, 6);
    if (FAILED(hr)) { why = "attributes"; return hr; }
    a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
    a->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, devMgr.Get());
    a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> w;
    hr = MFCreateSinkWriterFromURL(s.path.c_str(), nullptr, a.Get(), &w);
    if (FAILED(hr)) { why = "MFCreateSinkWriterFromURL (output folder writable?)"; return hr; }

    auto setColour = [](IMFMediaType* t) {
        t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    };
    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, c == VideoCodec::Av1 ? MFVideoFormat_AV1 : c == VideoCodec::Hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, outW, outH);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, c == VideoCodec::Av1    ? (UINT32)eAVEncAV1VProfile_Main_420_8
                                        : c == VideoCodec::Hevc ? (UINT32)eAVEncH265VProfile_Main_420_8
                                                                : (UINT32)eAVEncH264VProfile_High);
    setColour(out.Get());
    hr = w->AddStream(out.Get(), &vStream);
    if (FAILED(hr)) { why = "AddStream(video)"; return hr; }
    if (c == VideoCodec::Av1) {   // tests only: fail the AV1 attempt like a GPU without an AV1 encoder (real fallback path)
        wchar_t noAv1[8];
        if (GetEnvironmentVariableW(L"XR_EYE_CAPTURE_TEST_NO_AV1", noAv1, 8) > 0) { why = "test: XR_EYE_CAPTURE_TEST_NO_AV1"; return E_FAIL; }
    }

    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, outW, outH);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    setColour(in.Get());

    // Rate control (rate_control): vbr = unconstrained VBR at the mean bitrate (the behaviour before the key existed), cbr = CBR
    // at it, qp = Media Foundation's "quality" mode (eAVEncCommonRateControlMode_Quality) with a constant QP; bitrate_mbps is
    // not set at all there (the file size follows the content).
    // Constant QP, measured on an RTX 5090 (NVIDIA MFTs, encoder_selftest --detail: a noisy picture that changes every frame,
    // 960x540 per eye, 5 s at 90 fps; file sizes at qp 20 / qp 45):
    //   HEVC  : CODECAPI_AVEncVideoEncodeQP honoured (171 MB / 0.09 MB); the encoder reports back exactly that QP.
    //   H.264 : AVEncVideoEncodeQP honoured (160 MB / 0.03 MB), but only with CODECAPI_AVLowLatencyMode: without it the MFT
    //           keeps all 4 NV12 surfaces of the pool and the recording stalls after 4 frames (also when the QP is set through
    //           ICodecAPI on the transform). Low-latency mode is set for H.264 in qp mode only.
    //   AV1   : the MFT refuses AVEncVideoEncodeQP (SetInputMediaType: MF_E_INVALIDMEDIATYPE; ICodecAPI::SetValue on the
    //           transform: E_INVALIDARG; AVEncVideoEncodeFrameTypeQP and a 0..255 scale are refused the same way), so the
    //           second attempt sets CODECAPI_AVEncCommonQuality = 100 - qp * 100 / 51 instead: honoured (80 MB / 0.3 MB).
    //   AVEncCommonQuality must NOT be combined with AVEncVideoEncodeQP: the HEVC MFT then takes the quality value and
    //   ignores the QP (it reported QP 27 for qp 20 and QP 41 for qp 45).
    // CBR / VBR were not measured for size here (the existing VBR path is unchanged); both record and play.
    const RateControl rc = s.rateControl;
    const unsigned qp = s.qp > 51 ? 51u : s.qp;
    const UINT32 qualityFromQp = 100u - qp * 100u / 51u;
    const char* rcParams = rc == RateControl::Qp ? "constant-QP/GOP" : rc == RateControl::Cbr ? "CBR/bitrate/GOP" : "VBR/bitrate/GOP";
    // AV1 speed. Measured on an RTX 5090 (NVIDIA AV1 Encoder MFT, 5 s of 8K test content fed in real time): at the MFT's
    // default quality / speed it encodes only ~51 fps at 8192x4348 (~1.8 Gpixel/s), so a 72 / 90 Hz recording would drop
    // about a third of its frames; CODECAPI_AVEncCommonQualityVsSpeed <= 25 keeps 90 fps even at 8192x4632 (30 is the slow
    // setting again). So above 1 Gpixel/s (any 8K frame at 72 / 90 fps, 5760x3240 at 60) AV1 asks for 25: the slowest
    // setting that measured fast enough, with headroom for the game's own GPU load. Smaller frames keep the default
    // (4096x2304 at 90 fps = 0.85 Gpixel/s kept up at the default).
    // quality_vs_speed: an explicit 0..100 is set for every codec (and replaces the AV1 rule); auto (-1) keeps the rule.
    av1Speed = 0;
    qvsSet = -1;
    if (s.qualityVsSpeed >= 0) {
        qvsSet = s.qualityVsSpeed;
    } else if (c == VideoCodec::Av1 && (double)outW * outH * fps > 1.0e9) {
        av1Speed = 25;
        qvsSet = (int)av1Speed;
    }
    // qpAsQuality: rate_control = qp through AVEncCommonQuality (the second attempt; see above)
    auto encoderParams = [&](bool qpAsQuality) {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 6);
        if (rc == RateControl::Qp) {
            a->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
            if (qpAsQuality) {
                a->SetUINT32(CODECAPI_AVEncCommonQuality, qualityFromQp);
            } else {
                a->SetUINT64(CODECAPI_AVEncVideoEncodeQP, PackQp(qp));
                if (c == VideoCodec::H264) a->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            }
        } else {
            a->SetUINT32(CODECAPI_AVEncCommonRateControlMode, rc == RateControl::Cbr ? (UINT32)eAVEncCommonRateControlMode_CBR
                                                                                     : (UINT32)eAVEncCommonRateControlMode_UnconstrainedVBR);
            a->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
        }
        a->SetUINT32(CODECAPI_AVEncMPVGOPSize, fps * 2);
        if (qvsSet >= 0) a->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, (UINT32)qvsSet);
        return a;
    };
    char rcBuf[128];
    if (rc == RateControl::Qp) snprintf(rcBuf, sizeof(rcBuf), "constant QP %u", qp);
    else snprintf(rcBuf, sizeof(rcBuf), "%s %.0f Mb/s", RateControlName(rc), bitrate / 1e6);
    rcText = rcBuf;
    hr = w->SetInputMediaType(vStream, in.Get(), encoderParams(false).Get());
    if (FAILED(hr) && rc == RateControl::Qp) {
        LOGW("recorder: encoder rejected the quality mode with AVEncVideoEncodeQP %u (hr=0x%08lx) -- retrying with the quality "
             "mode and AVEncCommonQuality %u (= 100 - qp * 100 / 51)", qp, (unsigned long)hr, qualityFromQp);
        hr = w->SetInputMediaType(vStream, in.Get(), encoderParams(true).Get());
        if (SUCCEEDED(hr)) {
            snprintf(rcBuf, sizeof(rcBuf), "constant quality %u (from qp %u; this encoder takes no QP)", qualityFromQp, qp);
            rcText = rcBuf;
        }
    }
    if (FAILED(hr) && qvsSet >= 0) {   // keep the speed setting: without it 8K AV1 cannot keep VR frame rates
        LOGW("recorder: encoder rejected the %s parameters (hr=0x%08lx) -- retrying with only quality vs speed %u "
             "and the GOP size", rcParams, (unsigned long)hr, (unsigned)qvsSet);
        ComPtr<IMFAttributes> speed;
        MFCreateAttributes(&speed, 2);
        speed->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, (UINT32)qvsSet);
        speed->SetUINT32(CODECAPI_AVEncMPVGOPSize, fps * 2);
        hr = w->SetInputMediaType(vStream, in.Get(), speed.Get());
        if (SUCCEEDED(hr)) {
            rcText = std::string("encoder default (rate_control ") + RateControlKey(rc) + " not accepted)";
            LOGW("recorder: rate_control = %s was not accepted -- the encoder uses its default rate control", RateControlKey(rc));
        }
    }
    if (FAILED(hr)) {
        LOGW("recorder: encoder rejected the %s parameters (hr=0x%08lx) -- using its defaults", rc == RateControl::Vbr ? "VBR/GOP" : rcParams,
             (unsigned long)hr);
        if (av1Speed) {
            LOGW("recorder: the AV1 speed setting (quality vs speed %u) was not accepted -- 8K at VR frame rates may drop frames",
                 av1Speed);
            av1Speed = 0;
        } else if (qvsSet >= 0) {
            LOGW("recorder: quality_vs_speed = %d was not accepted -- the encoder uses its default", qvsSet);
        }
        qvsSet = -1;
        rcText = std::string("encoder default (rate_control ") + RateControlKey(rc) + " not accepted)";
        hr = w->SetInputMediaType(vStream, in.Get(), nullptr);
    }
    if (FAILED(hr)) { why = "SetInputMediaType(video NV12)"; return hr; }

    if (hasAudio) {
        ComPtr<IMFMediaType> ao, ai;
        MFCreateMediaType(&ao);
        ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aacBytes);
        hr = w->AddStream(ao.Get(), &aStream);
        if (FAILED(hr)) { why = "AddStream(audio)"; return hr; }
        MFCreateMediaType(&ai);
        ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        hr = w->SetInputMediaType(aStream, ai.Get(), nullptr);
        if (FAILED(hr)) { why = "SetInputMediaType(audio PCM)"; return hr; }
    }
    hr = w->BeginWriting();
    if (FAILED(hr)) { why = "BeginWriting"; return hr; }
    writer = w;
    if (av1Speed)
        LOGI("recorder: AV1 %ux%u at %u fps = %.2f Gpixel/s (> 1) -- encoder quality vs speed %u (its default speed cannot keep "
             "up with 8K at VR frame rates)", outW, outH, fps, (double)outW * outH * fps / 1e9, av1Speed);
    else if (qvsSet >= 0) {
        LOGI("recorder: encoder quality vs speed %d (quality_vs_speed; 0 = fastest, 100 = best quality)", qvsSet);
        if (c == VideoCodec::Av1 && qvsSet > 25 && (double)outW * outH * fps > 1.0e9)
            LOGW("recorder: AV1 %ux%u at %u fps = %.2f Gpixel/s with quality vs speed %d (> 25) -- the encoder may not keep up "
                 "(dropped frames); quality_vs_speed = auto uses 25 here", outW, outH, fps, (double)outW * outH * fps / 1e9, qvsSet);
    }
    if (rc == RateControl::Qp && rcText.compare(0, 9, "constant ") == 0)
        LOGI("recorder: rate_control = qp: %s (qp 0 = best, 51 = worst) -- bitrate_mbps is ignored, the file size follows the "
             "content", rcText.c_str());
    LOGI("recorder: sink writer ready (%s, %s transforms, rate control %s%s)", VideoCodecName(c), hardware ? "hardware" : "software",
         rcText.c_str(),
         hasAudio ? (aacBytes >= 32000 ? ", AAC 256 kb/s" : aacBytes >= 24000 ? ", AAC 192 kb/s" : ", AAC <192 kb/s") : ", no audio");
    return S_OK;
}

void Recorder::Impl::LogTransforms() {
    ComPtr<IMFSinkWriterEx> ex;
    if (!writer || FAILED(writer.As(&ex))) return;
    for (DWORD stream : {vStream, aStream}) {
        if (stream == aStream && !hasAudio) continue;
        for (DWORD i = 0; i < 8; ++i) {
            GUID cat{};
            ComPtr<IMFTransform> t;
            if (FAILED(ex->GetTransformForStream(stream, i, &cat, &t)) || !t) break;
            std::string name = "(no name)";
            bool hw = false, d3d11 = false;
            ComPtr<IMFAttributes> at;
            if (SUCCEEDED(t->GetAttributes(&at)) && at) {
                WCHAR* str = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(at->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &str, &len)) && str) {
                    name = elog::Narrow(str);
                    CoTaskMemFree(str);
                }
                UINT32 len2 = 0;
                hw = SUCCEEDED(at->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &len2));
                d3d11 = MFGetAttributeUINT32(at.Get(), MF_SA_D3D11_AWARE, 0) != 0;
            }
            LOGI("recorder: %s stream transform %lu: %s '%s'%s%s", stream == vStream ? "video" : "audio", (unsigned long)i,
                 GuidCategoryName(cat).c_str(), name.c_str(), hw ? " [hardware]" : "", d3d11 ? " [D3D11-aware]" : "");
        }
    }
}

// record_overlays: the per-slot overlay atlas (through the frame source: shared with the game device) + the compositor.
// Worker thread, at the first frame after the game thread saw an overlay layer; the frames before are recorded without
// their overlays (counted as "atlas not ready"). A failure switches overlays off for this recording.
void Recorder::Impl::InitOverlays() {
    const uint32_t want = ovWantMask.load() & ~(ovReadyMask.load() | ovFailedMask.load());
    if (!want) return;
    // view / shader / texture creation is free-threaded: nothing touches the immediate context here
    if (!ovComp.Ready() && !ovComp.Init(enc.Get(), ring.tex, fmt, srgb)) {
        ovFailedMask.fetch_or((1u << kOverlayFamilies) - 1);
        LOGE("recorder: overlay compositor setup failed -- overlays are not recorded in this recording");
        return;
    }
    const UINT A = kOverlayAtlasSize;
    for (int f = 0; f < kOverlayFamilies; ++f) {
        const uint32_t bit = 1u << f;
        if (!(want & bit)) continue;
        // typed member of the family (D3D11 cannot open a TYPELESS D3D12 shared resource; the shader decodes sRGB itself)
        const DXGI_FORMAT typed = OverlayFamilyFormat(f);
        const bool ok = typed != DXGI_FORMAT_UNKNOWN && src->CreateOverlayAtlas(enc.Get(), A, A, typed, kRingSize, f, ovAtlas[f]) &&
                        ovComp.AddAtlas(f, ovAtlas[f], typed);
        if (!ok) {
            ovFailedMask.fetch_or(bit);
            LOGE("recorder: overlay atlas setup failed (%s) -- layers of that format family are not recorded in this recording",
                 DxgiFormatName(typed));
            continue;
        }
        const double mb = (double)A * A * OverlayFamilyBytes(f) * kRingSize / (1024.0 * 1024.0);
        LOGI("recorder: overlays: %s atlas %ux%u %s x %u ring slots (+%.0f MB VRAM, shared with the game device) + compositor on "
             "the encoder device; blended in %s into the ring before %s", OverlayFamilyName(f), A, A, DxgiFormatName(typed),
             kRingSize, mb, srgb ? "linear light (sRGB decode / encode in the shader)" : "linear light",
             stage ? "the re-projection / dome pass" : "the encoder");
        ovReadyMask.fetch_or(bit, std::memory_order_release);
    }
}

void Recorder::Impl::Process(const Item& it) {
    if (!loggedFirst) {
        loggedFirst = true;
        LOGI("recorder: first video frame (t = 0 in the file): displayTime %lld ns", (long long)t0Disp.load());
    }
    encCtx4->Wait(ring.ready.Get(), it.ready);
    if (ovWantMask.load() & ~(ovReadyMask.load() | ovFailedMask.load())) InitOverlays();   // a new overlay format family

    // head smoothing, CPU part: filter + per-eye re-projection constants (before taking the device lock)
    RpConstants rc{};
    DomeConstants dc{};
    float headCorr[4] = {0, 0, 0, 1};   // O * H^-1 of this frame: head-locked overlays follow the smoothed head
    bool haveHeadCorr = false;
    int64_t cpuTicks = 0;   // our own CPU time on the worker (filter + constants + draw submission), not lock / pool waits
    if (stage && dome.valid) {
        const int64_t c0 = QpcNow100ns();
        DomeConstantsFor(it, dc);
        ++smFrames;
        cpuTicks += QpcNow100ns() - c0;
    } else if (stage) {
        const int64_t c0 = QpcNow100ns();
        SmoothEyeIn in[2];
        for (int e = 0; e < 2; ++e) {
            const ItemEye& ie = it.eye[e];
            in[e].valid = ie.valid;
            in[e].q.x = ie.q[0]; in[e].q.y = ie.q[1]; in[e].q.z = ie.q[2]; in[e].q.w = ie.q[3];
            in[e].tanL = std::tan((double)ie.fov[0]); in[e].tanR = std::tan((double)ie.fov[1]);
            in[e].tanU = std::tan((double)ie.fov[2]); in[e].tanD = std::tan((double)ie.fov[3]);
        }
        SmoothFrame sf;
        smoother.Update(it.disp, in, sf);
        ++smFrames;
        if (sf.guarded) ++smGuarded;
        if (sf.uncovered) ++smUncovered;
        if (sf.catchUp) ++smCatch;
        if (sf.noPose) ++smNoPose;
        if (sf.reset) ++smResets;
        smCorrSum += sf.correctionDeg;
        if (sf.correctionDeg > smCorrMax) smCorrMax = sf.correctionDeg;
        if (s.head.Enabled() && !sf.noPose) {   // smoothing off: overlays exactly as before (no correction at all)
            headCorr[0] = (float)sf.headCorr.x; headCorr[1] = (float)sf.headCorr.y;
            headCorr[2] = (float)sf.headCorr.z; headCorr[3] = (float)sf.headCorr.w;
            haveHeadCorr = true;
        }
        for (int e = 0; e < 2; ++e) {
            const SmoothEyeOut& eo = sf.eye[e];
            RpEye& re = rc.eye[e];
            for (int c = 0; c < 3; ++c) { re.r0[c] = eo.m[c]; re.r1[c] = eo.m[3 + c]; re.r2[c] = eo.m[6 + c]; }
            for (int c = 0; c < 4; ++c) { re.outTan[c] = eo.outTan[c]; re.srcTan[c] = eo.srcTan[c]; }
            const ItemEye& ie = it.eye[e];
            re.src[0] = (float)ie.x0; re.src[1] = 0.0f;
            re.src[2] = (float)std::max<UINT>(1, ie.w); re.src[3] = (float)std::max<UINT>(1, ie.h);
            if (yt) {   // YouTube layout: eye 0 left / top, eye 1 right / below (swap_eyes swapped the views before SubmitFrame);
                        // dst = the picture (4:3pad: vw wide at +vx, the bars around it black; else the eye's whole area)
                re.dst[0] = (float)((ytTb ? 0 : e * ytFit.sw) + ytFit.vx); re.dst[1] = (float)(ytTb ? e * ytFit.sh : 0);
                re.dst[2] = (float)ytFit.vw; re.dst[3] = (float)ytFit.sh;
            } else if (sym.valid) {
                re.dst[0] = (float)(e == 0 ? 0 : rpEyeW[0]); re.dst[1] = 0.0f;
                re.dst[2] = (float)rpEyeW[e]; re.dst[3] = (float)vpH;
            } else if (rpScaled) {   // raw layout, scaled: the source rectangle mapped into this eye's output rectangle
                const double sx = (double)rpEyeW[e] / std::max<UINT>(1, rpNativeEyeW[e]), sy = (double)vpH / std::max<UINT>(1, H);
                const double nx0 = e == 0 ? 0.0 : (double)rpNativeEyeW[0], ox0 = e == 0 ? 0.0 : (double)rpEyeW[0];
                re.dst[0] = (float)(ox0 + (re.src[0] - nx0) * sx); re.dst[1] = 0.0f;
                re.dst[2] = (float)(re.src[2] * sx); re.dst[3] = (float)(re.src[3] * sy);
            } else {
                for (int c = 0; c < 4; ++c) re.dst[c] = re.src[c];
            }
        }
        rc.misc[0] = 1.0f / (float)W; rc.misc[1] = 1.0f / (float)H; rc.misc[2] = (float)outRightX; rc.misc[3] = (float)rpTaps;
        rc.misc2[0] = (float)outBottomY;   // 0 = side by side: the eye pick is the old pos.x >= misc.z
        cpuTicks += QpcNow100ns() - c0;
    }

    // constant_frame_rate: the frame's slot on the 1/fps grid (integer maths: no float, no drift). A slot that already has
    // its frame drops this one; missing slots are filled with the previous frame BEFORE this frame's video processor pass
    // can reuse the NV12 surface that still holds the previous frame.
    int64_t cfrSlot = -1;
    if (cfr) {
        const int64_t rel0 = it.ts - t0.load();
        cfrSlot = rel0 < 0 ? -1 : (rel0 * (int64_t)fps + 5000000) / 10000000;
        if (cfrSlot <= cfrLastSlot) {
            ++cfrDropped;
            FreeSlot(it.slot, 0, false);   // no GPU work was queued on the slot
            return;
        }
        if (cfrLastSlot >= 0 && cfrSlot > cfrLastSlot + 1) CfrFill(cfrSlot);
    }

    const int n = pool->Acquire(3000);
    if (n < 0) {
        ++dropPool;
        if (dropPool.load() == 1) LOGW("recorder: encoder stalled for 3 s (no free NV12 surface) -- dropping frames");
        FreeSlot(it.slot, 0, false);
        return;
    }
    // Exclusive use of the encoder device's immediate context against the MF components that share it.
    ComPtr<ID3D11Device> locked;
    const bool haveLock = devHandle && SUCCEEDED(devMgr->LockDevice(devHandle, IID_PPV_ARGS(&locked), TRUE));
    // record_overlays: the overlay layers onto OUR copy of the eyes (the ring slot), before every other pass
    if (it.nOv && ovComp.Ready()) {
        OverlayEye oe[2];
        for (int e = 0; e < 2; ++e) {
            const ItemEye& ie = it.eye[e];
            oe[e].valid = ie.valid;
            memcpy(oe[e].q, ie.q, sizeof(oe[e].q));
            memcpy(oe[e].p, ie.p, sizeof(oe[e].p));
            memcpy(oe[e].fov, ie.fov, sizeof(oe[e].fov));
            oe[e].x0 = ie.x0; oe[e].w = ie.w; oe[e].h = ie.h;
        }
        const int nd = ovComp.Draw(encCtx.Get(), it.slot, oe, it.ov, it.nOv, it.nBehind, it.projUnpremul,
                                   haveHeadCorr ? headCorr : nullptr);
        if (nd) {
            ++ovFrames;
            ovDraws += (uint64_t)nd;
            if (!ovLoggedFirst) {
                ovLoggedFirst = true;
                LOGI("recorder: first frame with overlays composited (%u layer(s), %d eye draws) at t = %.3f s", it.nOv, nd,
                     (double)(it.ts - t0.load()) / 1e7);
            }
        }
    }
    ID3D11VideoProcessorInputView* view = nullptr;
    ID3D11ShaderResourceView* rpSrc = nullptr;
    if (!srgb) {
        ID3D11ShaderResourceView* srv = linSrv[it.slot].Get();
        ID3D11UnorderedAccessView* uav = linUav.Get();
        encCtx->CSSetShader(linCs.Get(), nullptr, 0);
        encCtx->CSSetShaderResources(0, 1, &srv);
        encCtx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        encCtx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        encCtx->CSSetShaderResources(0, 1, &nullSrv);
        encCtx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        encCtx->CSSetShader(nullptr, nullptr, 0);
        view = linView.Get();
        rpSrc = rpLinSrv.Get();
    } else if (stage) {
        rpSrc = rpRingSrv[it.slot].Get();
    } else {
        view = inViews[it.slot].Get();
        if (!localCopies.empty()) encCtx->CopyResource(localCopies[it.slot].Get(), ring.tex[it.slot].Get());
    }
    if (stage && dome.valid) {
        const int64_t c0 = QpcNow100ns();
        if (domeLastSeen) {
            if (domeClear) {   // record start: black dome, never painted
                const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                const float never[4] = {kDomeTimeReset, kDomeTimeReset, kDomeTimeReset, kDomeTimeReset};
                encCtx->ClearRenderTargetView(domeRtv[domeCur].Get(), black);
                encCtx->ClearRenderTargetView(domeTimeRtv[domeCur].Get(), never);
                domeClear = false;
            }
            const int nx = domeCur ^ 1;
            ID3D11ShaderResourceView* srvs[3] = {rpSrc, domeSrv[domeCur].Get(), domeTimeSrv[domeCur].Get()};
            ID3D11RenderTargetView* rtvs[3] = {rpRtv.Get(), domeRtv[nx].Get(), domeTimeRtv[nx].Get()};
            Reproject(srvs, 3, rtvs, 3, &dc, sizeof(dc));
            domeCur = nx;
        } else {
            ID3D11RenderTargetView* rtv = rpRtv.Get();
            Reproject(&rpSrc, 1, &rtv, 1, &dc, sizeof(dc));
        }
        view = rpView.Get();
        cpuTicks += QpcNow100ns() - c0;
    } else if (stage) {
        const int64_t c0 = QpcNow100ns();
        ID3D11RenderTargetView* rtv = rpRtv.Get();
        Reproject(&rpSrc, 1, &rtv, 1, &rc, sizeof(rc));   // after the pre-pass (linear formats), so the colour path is unchanged
        view = rpView.Get();
        cpuTicks += QpcNow100ns() - c0;
    }
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = view;
    HRESULT hr = vctx->VideoProcessorBlt(vp.Get(), outViews[(size_t)n].Get(), 0, 1, &st);
    ++freeCounter;
    encCtx4->Signal(ring.free.Get(), freeCounter);
    encCtx->Flush();
    if (haveLock) devMgr->UnlockDevice(devHandle, FALSE);
    if (stage) {
        const double ms = cpuTicks / 1e4;
        cpuSum += ms;
        if (ms > cpuMax) cpuMax = ms;
    }
    FreeSlot(it.slot, freeCounter, true);
    if (FAILED(hr)) {
        pool->Release(n);
        ++dropPool;
        if (n == cfrPrevIdx) cfrPrevIdx = -1;   // the surface may be half overwritten: no repeat source until the next frame
        LOGE("recorder: VideoProcessorBlt failed hr=0x%08lx", (unsigned long)hr);
        return;
    }

    if (cfr) {   // constant_frame_rate: written at its grid slot; this surface is now the repeat source
        cfrLastSlot = cfrRealSlot = cfrSlot;
        cfrGapCapped = false;
        cfrPrevIdx = WriteVideoSample(n, cfrSlot * 10000000LL / fps) ? n : -1;
        return;
    }
    WriteVideoSample(n, it.ts - t0.load());
}

// NV12 pool surface n -> one video sample at `time` (100 ns, relative to t0), duration 1 / fps. The tracked sample hands n
// back to the pool when Media Foundation releases it; on the paths that never hand it over, n is released here.
bool Recorder::Impl::WriteVideoSample(int n, int64_t time) {
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMF2DBuffer> b2;
    ComPtr<IMFTrackedSample> tracked;
    ComPtr<IMFSample> sample;
    DWORD len = 0;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12[(size_t)n].Get(), 0, FALSE, &buf);
    if (SUCCEEDED(hr)) hr = buf.As(&b2);
    if (SUCCEEDED(hr)) hr = b2->GetContiguousLength(&len);
    if (SUCCEEDED(hr)) hr = buf->SetCurrentLength(len);
    if (SUCCEEDED(hr)) hr = MFCreateTrackedSample(&tracked);
    if (SUCCEEDED(hr)) hr = tracked.As(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(10000000LL / fps);
    if (SUCCEEDED(hr)) hr = tracked->SetAllocator(poolCb[(size_t)n].Get(), nullptr);
    else pool->Release(n);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lk(wm);
        hr = writer ? writer->WriteSample(vStream, sample.Get()) : E_FAIL;
    }
    if (FAILED(hr)) {
        if (++writeErrors <= 5) LOGE("recorder: video WriteSample failed hr=0x%08lx", (unsigned long)hr);
        return false;
    }
    ++written;
    lastWrittenTs = time;
    return true;
}

// constant_frame_rate: fills the grid slots cfrLastSlot + 1 .. slot - 1 with the last written frame (cfrPrevIdx): a pool
// surface is acquired, the previous frame copied into it on the encoder device and written at the missing slot's time.
// The pool can hand out the very surface that holds the previous frame (the encoder released it, nothing overwrote it:
// only this thread writes pool surfaces): then it is written as it is. Each copy becomes the new repeat source. At most 5 s
// of frames after the last game frame (a frozen game must not write thousands of frames); the rest of a longer gap stays a
// gap. Called when a frame arrives after a gap, and by CfrIdleFill while no frame arrives.
void Recorder::Impl::CfrFill(int64_t slot) {
    if (cfrPrevIdx < 0 || cfrRealSlot < 0) return;
    const int64_t cap = 5 * (int64_t)fps;
    int64_t last = slot - 1;
    if (last > cfrRealSlot + cap) {
        last = cfrRealSlot + cap;
        if (!cfrGapCapped) {
            cfrGapCapped = true;
            if (++cfrCapped == 1)
                LOGW("recorder: constant frame rate: no frame from the game for more than 5 s -- the last frame was repeated for 5 s "
                     "(%lld frames); the rest of the gap stays a gap in the video (logged once)", (long long)cap);
        }
    }
    const int64_t missing = last - cfrLastSlot;
    for (int64_t j = 0; j < missing; ++j) {
        const int m = pool->Acquire(3000);
        if (m < 0) {
            ++dropPool;
            if (dropPool.load() == 1) LOGW("recorder: encoder stalled for 3 s (no free NV12 surface) -- dropping frames");
            return;
        }
        if (m != cfrPrevIdx) {
            ComPtr<ID3D11Device> locked;
            const bool haveLock = devHandle && SUCCEEDED(devMgr->LockDevice(devHandle, IID_PPV_ARGS(&locked), TRUE));
            encCtx->CopyResource(nv12[(size_t)m].Get(), nv12[(size_t)cfrPrevIdx].Get());
            encCtx->Flush();
            if (haveLock) devMgr->UnlockDevice(devHandle, FALSE);
        }
        const int64_t k = cfrLastSlot + 1;
        if (!WriteVideoSample(m, k * 10000000LL / fps)) { cfrPrevIdx = -1; return; }
        cfrLastSlot = k;
        cfrPrevIdx = m;
        ++cfrRepeated;
    }
}

// constant_frame_rate, worker idle (no frame for 20 ms): repeats the last frame into the slots whose time has clearly
// passed, so a stall is written while it happens instead of in one burst when the game resumes (a burst of hundreds of
// encodes would hold up the worker and make the game thread drop the frames that follow: measured 75 dropped frames after a
// 6 s stall at 3840x1080, many more at 8K). "Clearly passed" = older than now - 250 ms: a frame's time is its predicted
// display time, which is ahead of the moment it is submitted, so a frame that is still on its way never lands in a slot
// filled here (one that does is dropped like any second frame of a slot).
void Recorder::Impl::CfrIdleFill() {
    const int64_t t0v = t0.load();
    if (cfrPrevIdx < 0 || cfrRealSlot < 0 || t0v == INT64_MIN || cfrGapCapped) return;
    const int64_t rel = QpcNow100ns() - t0v - 2500000;
    if (rel <= 0) return;
    const int64_t passed = rel * (int64_t)fps / 10000000;   // the last slot whose time is over (floor)
    if (passed > cfrLastSlot) CfrFill(passed + 1);
}

void Recorder::Impl::OnAudio(const int16_t* pcm, uint32_t frames, int64_t startQpc) {
    const int64_t t0v = t0.load();
    if (t0v == INT64_MIN) return;               // video has not started yet
    int64_t rel = startQpc - t0v;
    if (rel < 0) {
        const uint64_t skip = (uint64_t)((-rel) * 48000 / 10000000LL) + 1;
        if (skip >= frames) return;
        pcm += skip * 2;
        frames -= (uint32_t)skip;
        rel += (int64_t)(skip * 10000000ull / 48000);
    }
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFSample> sample;
    BYTE* p = nullptr;
    const DWORD bytes = frames * 4;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf)) || FAILED(buf->Lock(&p, nullptr, nullptr))) return;
    memcpy(p, pcm, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    if (FAILED(MFCreateSample(&sample))) return;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(rel);
    sample->SetSampleDuration((LONGLONG)frames * 10000000LL / 48000);
    std::lock_guard<std::mutex> lk(wm);
    if (!writer) return;
    if (SUCCEEDED(writer->WriteSample(aStream, sample.Get()))) audioFrames += frames;
}

void Recorder::Impl::Cleanup() {
    if (hasAudio) audio.Stop();
    {
        std::lock_guard<std::mutex> lk(wm);
        writer.Reset();
    }
    if (devHandle && devMgr) devMgr->CloseDeviceHandle(devHandle);
    devHandle = nullptr;
    inViews.clear();
    localCopies.clear();
    ovComp.Release();
    for (auto& a : ovAtlas) a.clear();   // ovReadyMask stays for the summary line; Start resets it
    linView.Reset(); linUav.Reset(); linTex.Reset(); linSrv.clear(); linCs.Reset();
    rpView.Reset(); rpRtv.Reset(); rpTex.Reset(); rpRingSrv.clear(); rpLinSrv.Reset(); rpQ.clear();
    rpCb.Reset(); rpSmp.Reset(); rpVs.Reset(); rpPs.Reset();
    for (int i = 0; i < 2; ++i) {
        domeTex[i].Reset(); domeTimeTex[i].Reset(); domeRtv[i].Reset(); domeTimeRtv[i].Reset(); domeSrv[i].Reset(); domeTimeSrv[i].Reset();
    }
    outViews.clear();
    nv12.clear();
    poolCb.clear();
    vp.Reset();
    venum.Reset();
    vctx.Reset();
    vdev.Reset();
    if (ringCreated) {
        // The game thread no longer calls CopyEyes (state != Recording and gameBusy drained).
        const ULONGLONG t = GetTickCount64();
        while (gameBusy.load() > 0 && GetTickCount64() - t < 2000) Sleep(1);
        src->DestroyRing();
        ringCreated = false;
    }
    ring = SharedRing();
    if (encCtx) { encCtx->ClearState(); encCtx->Flush(); }
}
