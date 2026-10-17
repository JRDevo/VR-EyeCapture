// reproject -- the flat re-projection pass (symmetric fov / head-motion smoothing) shared by the recorder
// (src/recorder.cpp, on the encoder device) and the live share (src/live_share.cpp, on its share device): the shaders,
// the constant buffer layout and the small CPU helpers around HeadSmoother. The CPU maths is in head_smoothing.h; the
// dome pass (kDomePs) stays in recorder.cpp.
//
// One full-screen triangle over the whole SBS target; the pixel shader picks the eye by its destination rectangle. For
// an output pixel: ray in the smoothed (output) camera from the scaled fov -> C (actual-eye-from-smoothed rotation) ->
// project with the eye's actual fov -> sample that eye's half of the source bilinearly; outside it = black. The output is
// W x H (same layout as the source) or, with symmetric_fov, 2 x eyeW x eyeH of the common symmetric frustum -- either one
// scaled down to the recorded size when max_width / the codec limit asks for less (FitStageOutput). record_for_youtube
// (yt_layout.h): the two eyes of the symmetric frustum, vertically cropped (eye_crop), scaled and packed side by side or
// top / bottom into the YouTube frame -- the eye is picked by its destination rectangle (eye 1 = pos >= its x0 AND y0;
// for side-by-side y0 = 0, i.e. the old pick by x); eye_crop = 4:3pad renders the picture into a narrower rectangle
// (dst) centred in the eye's area, the rest of the area (uv outside 0..1) black. Source SRV and target RTV are the same
// typed UNORM / float format, so no gamma conversion happens. See head_smoothing.h.
#pragma once
#include <d3d11.h>
#include "frame_source.h"
#include "head_smoothing.h"

inline constexpr char kReprojVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
// misc.w = bilinear taps per axis (FitStageOutput): 1 = one bilinear sample (native size); n > 1 (the stage renders
// smaller than the source, max_width): an n x n grid over the output pixel's footprint in the source, from the same
// mapping evaluated one output pixel to the right / below, so a 4096 px eye shrunk to 640 px does not alias / shimmer.
// The footprint is per axis (dx, dy), so a layout that minifies one axis only (tb_half squeezes the height) is covered too.
inline constexpr char kReprojPs[] =
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

// A view pose / fov the re-projection can use: hasPose, finite fov, left < right, down < up, all within +-89 degrees.
bool ReprojPoseUsable(const EyeView& v);

// HeadSmoother input of one eye from its orientation (x, y, z, w) and fov (angleLeft, angleRight, angleUp, angleDown).
SmoothEyeIn ReprojSmoothIn(bool valid, const float q[4], const float fov[4]);

// The mapping part of one eye's constants (r0..r2 = C, outTan, srcTan) from the smoother's output; src / dst are the
// caller's (they depend on the layout).
void ReprojEyeMapping(const SmoothEyeOut& eo, RpEye& re);
// The identity mapping (C = I, outTan = srcTan): the pass then only scales the source rectangle into dst (the live share's
// "Half" size without re-projection, or overlays composited without smoothing / symmetric fov). At the same size it is
// an exact copy (every sample on a texel centre).
void ReprojIdentityMapping(RpEye& re);

// The pass itself on `ctx`: full pipeline state for one full-screen triangle over a w x h viewport (constant buffer at
// b0 of the pixel shader, sampler s0, SRVs t0.., RTVs 0..), Draw(3), then the SRVs / RTVs / shaders unbound again. The
// caller has written the constant buffer. ctx must be a context nobody else's state depends on (the recorder's encoder
// device, the live share's share device) -- never a game's or OBS's context.
void ReprojDraw(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11Buffer* cb,
                ID3D11SamplerState* smp, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
                ID3D11RenderTargetView* const* rtvs, UINT nRtv, UINT w, UINT h);
