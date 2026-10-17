// overlay -- see overlay.h.
#include "overlay.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "log.h"

namespace {

// Vertex: clip-space position + uv of the sub-image. Pixel: a bilinear sample of the atlas done by hand (4 loads, each
// sRGB-decoded first when the layer's swapchain is _SRGB -- what a runtime's _SRGB view does; the atlas is a typed
// UNORM / float texture because D3D11 cannot open a typeless D3D12 shared resource), texel coordinates clamped to the
// sub-image (cube: the face) so neighbours in the atlas never bleed in; colour scale / bias; the destination from the
// scratch copy; blend in linear light.
//   psmain: quad / cylinder mesh (uv from the vertices).
//   psray : equirect / equirect2 / cube: a full-eye draw; the pixel's ray (eye fov) rotated into the layer frame, then
//           the sphere intersection (finite radius) and the equirect mapping, or the cube face lookup (TextureCube rules:
//           major axis, face 0..5 = +X -X +Y -Y +Z -Z, the faces laid out 3 x 2 in the atlas). Uncovered pixels: discard.
//   psover: the saved projection eye blended OVER what the layers behind it left in the ring (its own alpha).
// cbuffer: rect = the sub-image in atlas pixels (x, y, w, h; cube: block origin + face size); dst.xy = the scratch
// copy's origin in the ring slot (pixels); flags = (source alpha, unpremultiplied, ring sRGB-encoded, source
// sRGB-encoded); cscale / cbias = XrCompositionLayerColorScaleBiasKHR; eye = (x0, w, h, mode 1 equirect / 2 equirect2 /
// 3 cube); tans = the eye's tanL, tanR, tanU, tanD; r0..r2 = eye camera -> layer frame rows, r0.w = sphere radius (0 =
// infinite); org = the eye position in the layer frame; eq = equirect (scale.xy, bias.xy) / equirect2 (central angle,
// upper, lower).
const char kOverlayHlsl[] =
    "struct VSIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vsmain(VSIn i) { VSOut o; o.pos = i.pos; o.uv = i.uv; return o; }\n"
    "cbuffer C : register(b0) { float4 rect; float4 dst; float4 flags; float4 cscale; float4 cbias; float4 eye; float4 tans;\n"
    "                           float4 r0; float4 r1; float4 r2; float4 org; float4 eq; float4 pad0; float4 pad1; };\n"
    "Texture2D<float4> src : register(t0);\n"
    "Texture2D<float4> under : register(t1);\n"
    "Texture2D<float4> proj : register(t2);\n"
    "float3 Dec(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }\n"
    "float3 Enc(float3 l) { l = saturate(l); return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }\n"
    "float4 Tex(int2 p, float4 r) {\n"
    "    float4 c = src.Load(int3(clamp(p, int2(r.xy), int2(r.xy + r.zw) - 1), 0));\n"
    "    if (flags.w > 0.5) c.rgb = Dec(c.rgb);\n"
    "    return c;\n"
    "}\n"
    "float4 Bilin(float2 uv, float4 r) {\n"
    "    float2 q = r.xy + uv * r.zw - 0.5;\n"
    "    int2 p0 = int2(floor(q));\n"
    "    float2 f = q - (float2)p0;\n"
    "    return lerp(lerp(Tex(p0, r), Tex(p0 + int2(1, 0), r), f.x), lerp(Tex(p0 + int2(0, 1), r), Tex(p0 + int2(1, 1), r), f.x), f.y);\n"
    "}\n"
    "float4 Blend(float4 s, float2 pos) {\n"
    "    s = s * cscale + cbias;\n"
    "    float4 d = under.Load(int3(int2(pos) - int2(dst.xy), 0));\n"
    "    float3 dl = flags.z > 0.5 ? Dec(d.rgb) : d.rgb;\n"
    "    float a = flags.x > 0.5 ? saturate(s.a) : 1.0;\n"
    "    float3 c = (flags.x > 0.5 && flags.y > 0.5) ? s.rgb * a : s.rgb;\n"
    "    float3 o = c + dl * (1.0 - a);\n"
    "    return float4(flags.z > 0.5 ? Enc(o) : o, d.a);\n"
    "}\n"
    "float4 psmain(VSOut i) : SV_Target { return Blend(Bilin(i.uv, rect), i.pos.xy); }\n"
    "float4 psray(VSOut i) : SV_Target {\n"
    "    float2 e = (i.pos.xy - float2(eye.x, 0.0)) / eye.yz;\n"
    "    float3 de = float3(lerp(tans.x, tans.y, e.x), lerp(tans.z, tans.w, e.y), -1.0);\n"
    "    float3 d = normalize(float3(dot(r0.xyz, de), dot(r1.xyz, de), dot(r2.xyz, de)));\n"
    "    float4 s;\n"
    "    if (eye.w > 2.5) {\n"
    "        float3 a = abs(d);\n"
    "        int f; float2 st; float ma;\n"
    "        if (a.x >= a.y && a.x >= a.z) { ma = a.x; f = d.x > 0.0 ? 0 : 1; st = d.x > 0.0 ? float2(-d.z, -d.y) : float2(d.z, -d.y); }\n"
    "        else if (a.y >= a.z) { ma = a.y; f = d.y > 0.0 ? 2 : 3; st = d.y > 0.0 ? float2(d.x, d.z) : float2(d.x, -d.z); }\n"
    "        else { ma = a.z; f = d.z > 0.0 ? 4 : 5; st = d.z > 0.0 ? float2(d.x, -d.y) : float2(-d.x, -d.y); }\n"
    "        float2 uv = 0.5 * (st / ma + 1.0);\n"
    "        float4 fr = float4(rect.xy + float2((float)(f % 3), (float)(f / 3)) * rect.zw, rect.zw);\n"
    "        s = Bilin(uv, fr);\n"
    "    } else {\n"
    "        if (r0.w > 0.0) {\n"
    "            float b = dot(org.xyz, d), c = dot(org.xyz, org.xyz) - r0.w * r0.w, disc = b * b - c;\n"
    "            if (disc < 0.0) discard;\n"
    "            float t = -b + sqrt(disc);\n"
    "            if (t <= 0.0) discard;\n"
    "            d = normalize(org.xyz + t * d);\n"
    "        }\n"
    "        float az = atan2(d.x, -d.z), el = asin(clamp(d.y, -1.0, 1.0));\n"
    "        float2 uv;\n"
    "        if (eye.w > 1.5) {\n"
    "            if (abs(az) > 0.5 * eq.x || el > eq.y || el < eq.z) discard;\n"
    "            uv = float2(0.5 + az / eq.x, (eq.y - el) / (eq.y - eq.z));\n"
    "        } else {\n"
    "            uv = float2(az * 0.1591549431 + 0.5, 0.5 - el * 0.3183098862) * eq.xy + eq.zw;\n"
    "            if (any(uv < 0.0) || any(uv > 1.0)) discard;\n"
    "        }\n"
    "        s = Bilin(uv, rect);\n"
    "    }\n"
    "    return Blend(s, i.pos.xy);\n"
    "}\n"
    "float4 psover(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.pos.xy);\n"
    "    float4 P = proj.Load(int3(p - int2((int)eye.x, 0), 0));\n"
    "    float4 B = under.Load(int3(p - int2(dst.xy), 0));\n"
    "    float3 pl = flags.z > 0.5 ? Dec(P.rgb) : P.rgb;\n"
    "    float3 bl = flags.z > 0.5 ? Dec(B.rgb) : B.rgb;\n"
    "    float a = saturate(P.a);\n"
    "    float3 c = flags.y > 0.5 ? pl * a : pl;\n"
    "    float3 o = c + bl * (1.0 - a);\n"
    "    return float4(flags.z > 0.5 ? Enc(o) : o, 1.0);\n"
    "}\n";
struct OverlayCb {
    float rect[4], dst[4], flags[4], cscale[4], cbias[4], eye[4], tans[4], r0[4], r1[4], r2[4], org[4], eq[4], pad0[4], pad1[4];
};
static_assert(sizeof(OverlayCb) % 16 == 0, "constant buffer size");

struct V3 { double x, y, z; };
V3 Cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// v rotated by the unit quaternion (qx, qy, qz, qw); inverse = the conjugate
V3 Rotate(const float* q, const V3& v, bool inverse) {
    const double s = inverse ? -1.0 : 1.0;
    const V3 u{s * q[0], s * q[1], s * q[2]};
    const V3 c = Cross(u, v);
    const V3 t{2 * c.x, 2 * c.y, 2 * c.z};
    const V3 c2 = Cross(u, t);
    return {v.x + q[3] * t.x + c2.x, v.y + q[3] * t.y + c2.y, v.z + q[3] * t.z + c2.z};
}
bool Finite(double v) { return std::isfinite(v); }

const double kNear = 0.01;   // metres: anything nearer than 1 cm to the eye is clipped

struct FamilyInfo { DXGI_FORMAT typed; const char* name; UINT bytes; };
const FamilyInfo kFamilies[kOverlayFamilies] = {
    {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8", 4},      {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8", 4},
    {DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8", 4},      {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2", 4},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 8}, {DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 4},
};

// The eye's tangents; false = unusable fov.
bool EyeTans(const OverlayEye& e, double& tl, double& tr, double& tu, double& td) {
    tl = std::tan((double)e.fov[0]); tr = std::tan((double)e.fov[1]);
    tu = std::tan((double)e.fov[2]); td = std::tan((double)e.fov[3]);
    return tr - tl > 1e-6 && tu - td > 1e-6 && Finite(tr - tl) && Finite(tu - td);
}

} // namespace

const char* OverlayKindName(OverlayKind k) {
    switch (k) {
    case OverlayKind::Quad: return "quad";
    case OverlayKind::Cylinder: return "cylinder";
    case OverlayKind::Equirect: return "equirect";
    case OverlayKind::Equirect2: return "equirect2";
    case OverlayKind::Cube: return "cube";
    }
    return "?";
}

DXGI_FORMAT TypelessForDxgi(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

int OverlayFamily(DXGI_FORMAT f) {
    switch (TypelessForDxgi(f)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return 0;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return 1;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return 2;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 3;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 4;
    case DXGI_FORMAT_R11G11B10_FLOAT: return 5;
    default: return -1;
    }
}
DXGI_FORMAT OverlayFamilyFormat(int family) { return family >= 0 && family < kOverlayFamilies ? kFamilies[family].typed : DXGI_FORMAT_UNKNOWN; }
const char* OverlayFamilyName(int family) { return family >= 0 && family < kOverlayFamilies ? kFamilies[family].name : "none"; }
UINT OverlayFamilyBytes(int family) { return family >= 0 && family < kOverlayFamilies ? kFamilies[family].bytes : 0; }

int OverlayClipVertices(const OverlayLayer& l, const OverlayEye& e, OverlayVertex* out, int maxVerts) {
    if (!e.valid || !out || OverlayKindIsRay(l.kind)) return 0;
    double tl, tr, tu, td;
    if (!EyeTans(e, tl, tr, tu, td)) return 0;
    struct Lv { double x, y, z, u, v; };
    Lv loc[kOverlayMaxVertices];
    int n = 0;
    if (l.kind == OverlayKind::Quad) {
        const double hw = 0.5 * l.width, hh = 0.5 * l.height;
        if (!(hw > 0) || !(hh > 0) || !Finite(hw) || !Finite(hh)) return 0;
        // strip BL, TL, BR, TR: clockwise on screen seen from +Z (the front face), so back faces are culled
        loc[n++] = {-hw, -hh, 0, 0, 1};
        loc[n++] = {-hw, hh, 0, 0, 0};
        loc[n++] = {hw, -hh, 0, 1, 1};
        loc[n++] = {hw, hh, 0, 1, 0};
    } else {
        const double r = l.radius, c = l.centralAngle, a = l.aspect;
        if (!(r > 0) || !(c > 0) || !(a > 0) || !Finite(r) || !Finite(c) || !Finite(a) || c > 6.2832) return 0;
        const double hh = 0.5 * r * c / a;   // XR_KHR_composition_layer_cylinder: height = radius * centralAngle / aspectRatio
        int segs = (int)std::ceil(c / 0.035);   // ~2 degrees per segment
        segs = std::max(2, std::min(64, segs));
        if (segs & 1) ++segs;                // even: the arc's centre is a vertex column
        if (segs > 64) segs = 64;
        for (int i = 0; i <= segs; ++i) {
            const double th = -0.5 * c + c * i / segs, u = (double)i / segs;
            const double x = r * std::sin(th), z = -r * std::cos(th);
            loc[n++] = {x, -hh, z, u, 1};    // bottom, top: clockwise seen from the inside (the visible side)
            loc[n++] = {x, hh, z, u, 0};
        }
    }
    if (n > maxVerts) return 0;
    for (int i = 0; i < n; ++i) {
        V3 P = Rotate(l.q, {loc[i].x, loc[i].y, loc[i].z}, false);
        P.x += l.p[0]; P.y += l.p[1]; P.z += l.p[2];
        const V3 E = Rotate(e.q, {P.x - e.p[0], P.y - e.p[1], P.z - e.p[2]}, true);
        const double w = -E.z;
        OverlayVertex& o = out[i];
        o.x = (float)((2.0 * E.x + (tr + tl) * E.z) / (tr - tl));
        o.y = (float)((2.0 * E.y + (tu + td) * E.z) / (tu - td));
        o.z = (float)(w - kNear);
        o.w = (float)w;
        o.u = (float)loc[i].u;
        o.v = (float)loc[i].v;
        if (!Finite(o.x) || !Finite(o.y) || !Finite(o.w)) return 0;
    }
    return n;
}

bool OverlayPixelBox(const OverlayVertex* v, int n, const OverlayEye& e, UINT& x0, UINT& y0, UINT& x1, UINT& y1) {
    if (n <= 0 || !e.w || !e.h) return false;
    double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30;
    bool behind = false;
    for (int i = 0; i < n; ++i) {
        if (v[i].w < kNear) { behind = true; break; }
        const double px = e.x0 + (0.5 + 0.5 * v[i].x / v[i].w) * e.w;
        const double py = (0.5 - 0.5 * v[i].y / v[i].w) * e.h;
        mnx = std::min(mnx, px); mxx = std::max(mxx, px);
        mny = std::min(mny, py); mxy = std::max(mxy, py);
    }
    const double ex0 = e.x0, ex1 = (double)e.x0 + e.w, ey1 = e.h;
    if (behind) { mnx = ex0; mxx = ex1; mny = 0; mxy = ey1; }
    mnx = std::max(ex0, std::floor(mnx) - 1); mxx = std::min(ex1, std::ceil(mxx) + 1);
    mny = std::max(0.0, std::floor(mny) - 1); mxy = std::min(ey1, std::ceil(mxy) + 1);
    if (!(mxx > mnx) || !(mxy > mny)) return false;
    x0 = (UINT)mnx; x1 = (UINT)mxx; y0 = (UINT)mny; y1 = (UINT)mxy;
    return x1 > x0 && y1 > y0;
}

void OverlayHeadLock(OverlayLayer& l, const OverlayEye& e, const float c[4]) {
    const V3 d = Rotate(c, {(double)l.p[0] - e.p[0], (double)l.p[1] - e.p[1], (double)l.p[2] - e.p[2]}, false);
    l.p[0] = (float)(e.p[0] + d.x); l.p[1] = (float)(e.p[1] + d.y); l.p[2] = (float)(e.p[2] + d.z);
    const double ax = c[0], ay = c[1], az = c[2], aw = c[3], bx = l.q[0], by = l.q[1], bz = l.q[2], bw = l.q[3];
    double q[4] = {aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                   aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz};   // corr * q
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-9) for (int i = 0; i < 4; ++i) l.q[i] = (float)(q[i] / n);
}

bool OverlayRayFor(const OverlayLayer& l, const OverlayEye& e, OverlayRay& out) {
    if (!e.valid || !OverlayKindIsRay(l.kind)) return false;
    // column j = eye basis vector j, rotated into the projection space by the eye, then into the layer frame
    const V3 basis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int j = 0; j < 3; ++j) {
        const V3 v = Rotate(l.q, Rotate(e.q, basis[j], false), true);
        out.r[0][j] = (float)v.x; out.r[1][j] = (float)v.y; out.r[2][j] = (float)v.z;
    }
    const V3 o = Rotate(l.q, {(double)e.p[0] - l.p[0], (double)e.p[1] - l.p[1], (double)e.p[2] - l.p[2]}, true);
    out.o[0] = (float)o.x; out.o[1] = (float)o.y; out.o[2] = (float)o.z;
    for (int i = 0; i < 3; ++i) {
        if (!Finite(out.o[i])) return false;
        for (int j = 0; j < 3; ++j) if (!Finite(out.r[i][j])) return false;
    }
    return true;
}

bool OverlayRayLookup(const OverlayLayer& l, const OverlayEye& e, double px, double py, double& u, double& v, int& face) {
    face = -1;
    double tl, tr, tu, td;
    OverlayRay ray;
    if (!e.w || !e.h || !EyeTans(e, tl, tr, tu, td) || !OverlayRayFor(l, e, ray)) return false;
    const double ex = (px - e.x0) / e.w, ey = py / e.h;
    if (ex < 0 || ex > 1 || ey < 0 || ey > 1) return false;
    const double de[3] = {tl + ex * (tr - tl), tu + ey * (td - tu), -1.0};
    double d[3];
    for (int i = 0; i < 3; ++i) d[i] = ray.r[i][0] * de[0] + ray.r[i][1] * de[1] + ray.r[i][2] * de[2];
    double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (double& c : d) c /= n;
    if (l.kind == OverlayKind::Cube) {
        const double a[3] = {std::fabs(d[0]), std::fabs(d[1]), std::fabs(d[2])};
        double ma, s, t;
        if (a[0] >= a[1] && a[0] >= a[2]) { ma = a[0]; face = d[0] > 0 ? 0 : 1; s = d[0] > 0 ? -d[2] : d[2]; t = -d[1]; }
        else if (a[1] >= a[2]) { ma = a[1]; face = d[1] > 0 ? 2 : 3; s = d[0]; t = d[1] > 0 ? d[2] : -d[2]; }
        else { ma = a[2]; face = d[2] > 0 ? 4 : 5; s = d[2] > 0 ? d[0] : -d[0]; t = -d[1]; }
        u = 0.5 * (s / ma + 1);
        v = 0.5 * (t / ma + 1);
        return true;
    }
    const double r = (l.radius > 0 && Finite(l.radius)) ? (double)l.radius : 0.0;
    if (r > 0) {
        const double b = ray.o[0] * d[0] + ray.o[1] * d[1] + ray.o[2] * d[2];
        const double c = ray.o[0] * ray.o[0] + ray.o[1] * ray.o[1] + ray.o[2] * ray.o[2] - r * r;
        const double disc = b * b - c;
        if (disc < 0) return false;
        const double t = -b + std::sqrt(disc);
        if (t <= 0) return false;
        for (int i = 0; i < 3; ++i) d[i] = ray.o[i] + t * d[i];
        n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (double& c2 : d) c2 /= n;
    }
    const double az = std::atan2(d[0], -d[2]), el = std::asin(std::max(-1.0, std::min(1.0, d[1])));
    if (l.kind == OverlayKind::Equirect2) {
        const double ch = l.centralHorizontalAngle, up = l.upperVerticalAngle, lo = l.lowerVerticalAngle;
        if (std::fabs(az) > 0.5 * ch || el > up || el < lo) return false;
        u = 0.5 + az / ch;
        v = (up - el) / (up - lo);
        return true;
    }
    u = (az / (2 * 3.14159265358979) + 0.5) * l.eqScale[0] + l.eqBias[0];
    v = (0.5 - el / 3.14159265358979) * l.eqScale[1] + l.eqBias[1];
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

bool OverlayCompositor::Init(ID3D11Device* dev, const std::vector<ComPtr<ID3D11Texture2D>>& ring, DXGI_FORMAT ringFmt,
                             bool ringSrgb) {
    Release();
    if (!dev || ring.empty()) return false;
    dev_ = dev;
    ring_ = ring;
    ringFmt_ = ringFmt;
    ringSrgb_ = ringSrgb;
    D3D11_TEXTURE2D_DESC rd{};
    ring_[0]->GetDesc(&rd);
    ringW_ = rd.Width;
    ringH_ = rd.Height;

    ComPtr<ID3DBlob> vsb, psb, psrb, psob, errs;
    const char* stage = "vsmain";
    HRESULT hr = D3DCompile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "overlay", nullptr, nullptr, "vsmain", "vs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &errs);
    if (SUCCEEDED(hr)) {
        stage = "psmain";
        errs.Reset();
        hr = D3DCompile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "overlay", nullptr, nullptr, "psmain", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &errs);
    }
    if (SUCCEEDED(hr)) {
        stage = "psray";
        errs.Reset();
        hr = D3DCompile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "overlay", nullptr, nullptr, "psray", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psrb, &errs);
    }
    if (SUCCEEDED(hr)) {
        stage = "psover";
        errs.Reset();
        hr = D3DCompile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "overlay", nullptr, nullptr, "psover", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psob, &errs);
    }
    if (FAILED(hr)) {
        LOGE("overlays: shader compile (%s) failed hr=0x%08lx %s", stage, (unsigned long)hr,
             errs ? (const char*)errs->GetBufferPointer() : "");
        Release();
        return false;
    }
    hr = dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(psrb->GetBufferPointer(), psrb->GetBufferSize(), nullptr, &psRay_);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(psob->GetBufferPointer(), psob->GetBufferSize(), nullptr, &psOver_);
    if (SUCCEEDED(hr)) {
        const D3D11_INPUT_ELEMENT_DESC ie[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        hr = dev->CreateInputLayout(ie, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il_);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(sizeof(OverlayVertex) * kOverlayMaxVertices);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = dev->CreateBuffer(&bd, nullptr, &vb_);
        if (SUCCEEDED(hr)) {
            bd.ByteWidth = sizeof(OverlayCb);
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            hr = dev->CreateBuffer(&bd, nullptr, &cb_);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_BACK;          // quads are one-sided (front = +Z), cylinders show their inside
        r.FrontCounterClockwise = FALSE;
        r.DepthClipEnable = TRUE;              // the near plane (z = w - 1 cm)
        hr = dev->CreateRasterizerState(&r, &rs_);
    }
    for (size_t i = 0; SUCCEEDED(hr) && i < ring_.size(); ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC v{};
        v.Format = ringFmt;
        v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = dev->CreateRenderTargetView(ring_[i].Get(), &v, &rtv);
        ringRtv_.push_back(rtv);
    }
    if (FAILED(hr)) {
        LOGE("overlays: compositor setup on the encoder device failed hr=0x%08lx", (unsigned long)hr);
        Release();
        return false;
    }
    ready_ = true;
    return true;
}

bool OverlayCompositor::AddAtlas(int family, const std::vector<ComPtr<ID3D11Texture2D>>& atlas, DXGI_FORMAT atlasFmt) {
    if (!ready_ || family < 0 || family >= kOverlayFamilies || atlas.size() < ring_.size()) return false;
    std::vector<ComPtr<ID3D11ShaderResourceView>> srvs;
    for (size_t i = 0; i < ring_.size(); ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = atlasFmt;   // typed UNORM / float: the shader decodes sRGB itself
        ComPtr<ID3D11ShaderResourceView> u;
        const HRESULT hr = dev_->CreateShaderResourceView(atlas[i].Get(), &sd, &u);
        if (FAILED(hr)) {
            LOGE("overlays: atlas view (%s) on the encoder device failed hr=0x%08lx", DxgiFormatName(atlasFmt), (unsigned long)hr);
            return false;
        }
        srvs.push_back(u);
    }
    atlasSrv_[family] = std::move(srvs);
    return true;
}

bool OverlayCompositor::EnsureTex(ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11ShaderResourceView>& v, UINT& tw, UINT& th,
                                  UINT w, UINT h, const char* what) {
    if (t && w <= tw && h <= th) return true;
    const UINT nw = std::min(ringW_, std::max(tw, (w + 63) & ~63u));
    const UINT nh = std::min(ringH_, std::max(th, (h + 63) & ~63u));
    if (nw < w || nh < h) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = nw; d.Height = nh; d.MipLevels = 1; d.ArraySize = 1; d.Format = ringFmt_;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> nt;
    ComPtr<ID3D11ShaderResourceView> nv;
    HRESULT hr = dev_->CreateTexture2D(&d, nullptr, &nt);
    if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(nt.Get(), nullptr, &nv);
    if (FAILED(hr)) {
        if (!loggedScratch_) { loggedScratch_ = true; LOGE("overlays: %s texture %ux%u failed hr=0x%08lx", what, nw, nh, (unsigned long)hr); }
        return false;
    }
    t = nt;
    v = nv;
    tw = nw;
    th = nh;
    LOGD("overlays: %s texture now %ux%u %s", what, nw, nh, DxgiFormatName(ringFmt_));
    return true;
}

double OverlayCompositor::ScratchMB() const {
    return ((double)scratchW_ * scratchH_ + (double)projW_ * projH_) * (ringFmt_ == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4) /
           (1024.0 * 1024.0);
}

bool OverlayCompositor::Bind(ID3D11DeviceContext* ctx) {
    if (bound_) return true;
    const UINT stride = sizeof(OverlayVertex), offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    ID3D11Buffer* cb = cb_.Get();
    ctx->IASetInputLayout(il_.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->RSSetState(rs_.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    bound_ = true;
    return true;
}

namespace {
// The whole eye: a clip-space rectangle (strip BL, TL, BR, TR = clockwise, front-facing).
int FullRect(OverlayVertex* v) {
    v[0] = {-1, -1, 0.5f, 1, 0, 1};
    v[1] = {-1, 1, 0.5f, 1, 0, 0};
    v[2] = {1, -1, 0.5f, 1, 1, 1};
    v[3] = {1, 1, 0.5f, 1, 1, 0};
    return 4;
}
bool Upload(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    memcpy(ms.pData, data, bytes);
    ctx->Unmap(b, 0);
    return true;
}
void EyeCb(OverlayCb& c, const OverlayEye& E) {
    c.eye[0] = (float)E.x0; c.eye[1] = (float)E.w; c.eye[2] = (float)E.h;
    c.tans[0] = std::tan(E.fov[0]); c.tans[1] = std::tan(E.fov[1]); c.tans[2] = std::tan(E.fov[2]); c.tans[3] = std::tan(E.fov[3]);
}
} // namespace

// One layer into one eye. Returns 1 if drawn.
bool OverlayCompositor::DrawLayer(ID3D11DeviceContext* ctx, UINT slot, const OverlayEye& E, int e, const OverlayDraw& dr, uint32_t k,
                                  const float* headCorr) {
    if (!(dr.l.eyeMask & (1u << e)) || !HasFamily(dr.l.family)) return false;
    // head smoothing: a head-locked layer moves with the output (smoothed) camera instead of the actual head
    OverlayLayer locked;
    if (headCorr && dr.l.headLocked) {
        locked = dr.l;
        OverlayHeadLock(locked, E, headCorr);
    }
    const OverlayLayer& l = headCorr && dr.l.headLocked ? locked : dr.l;
    OverlayVertex v[kOverlayMaxVertices];
    OverlayCb c{};
    EyeCb(c, E);
    int nv = 0;
    UINT bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool boxOk = false;
    const bool ray = OverlayKindIsRay(l.kind);
    if (ray) {
        OverlayRay r;
        double tl, tr, tu, td;
        if (OverlayRayFor(l, E, r) && EyeTans(E, tl, tr, tu, td)) {
            nv = FullRect(v);
            bx0 = E.x0; by0 = 0; bx1 = E.x0 + E.w; by1 = E.h;
            boxOk = true;
            for (int i = 0; i < 3; ++i) { c.r0[i] = r.r[0][i]; c.r1[i] = r.r[1][i]; c.r2[i] = r.r[2][i]; c.org[i] = r.o[i]; }
            c.r0[3] = (l.kind != OverlayKind::Cube && l.radius > 0 && std::isfinite(l.radius)) ? l.radius : 0.0f;
            c.eye[3] = l.kind == OverlayKind::Equirect ? 1.0f : l.kind == OverlayKind::Equirect2 ? 2.0f : 3.0f;
            if (l.kind == OverlayKind::Equirect) {
                c.eq[0] = l.eqScale[0]; c.eq[1] = l.eqScale[1]; c.eq[2] = l.eqBias[0]; c.eq[3] = l.eqBias[1];
            } else {
                c.eq[0] = l.centralHorizontalAngle; c.eq[1] = l.upperVerticalAngle; c.eq[2] = l.lowerVerticalAngle;
            }
        }
    } else {
        nv = OverlayClipVertices(l, E, v, kOverlayMaxVertices);
        boxOk = nv && OverlayPixelBox(v, nv, E, bx0, by0, bx1, by1);
    }
    if (debugDraws_ > 0) {
        --debugDraws_;
        LOGD("overlays: draw slot %u layer %u (%s%s) eye %d: %d vertices, pixel box %u,%u..%u,%u%s, atlas %s %u,%u %ux%u", slot, k,
             OverlayKindName(l.kind), l.behind ? ", behind" : "", e, nv, bx0, by0, bx1, by1, boxOk ? "" : " (nothing to draw)",
             OverlayFamilyName(l.family), dr.ax, dr.ay, l.w, l.h);
    }
    if (!boxOk) return false;
    if (bx1 > ringW_ || by1 > ringH_ || !EnsureTex(scratch_, scratchSrv_, scratchW_, scratchH_, bx1 - bx0, by1 - by0, "destination scratch"))
        return false;
    Bind(ctx);
    // the destination under the layer -> scratch (the ring slot must not be bound while it is the copy source)
    ID3D11ShaderResourceView* nullSrv[3] = {};
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->PSSetShaderResources(0, 3, nullSrv);
    const D3D11_BOX box{bx0, by0, 0, bx1, by1, 1};
    ctx->CopySubresourceRegion(scratch_.Get(), 0, 0, 0, 0, ring_[slot].Get(), 0, &box);
    if (!Upload(ctx, vb_.Get(), v, sizeof(OverlayVertex) * (size_t)nv)) return false;
    c.rect[0] = (float)dr.ax; c.rect[1] = (float)dr.ay; c.rect[2] = (float)l.w; c.rect[3] = (float)l.h;
    c.dst[0] = (float)bx0; c.dst[1] = (float)by0;
    c.flags[0] = l.alpha ? 1.0f : 0.0f;
    c.flags[1] = l.unpremultiplied ? 1.0f : 0.0f;
    c.flags[2] = ringSrgb_ ? 1.0f : 0.0f;
    c.flags[3] = l.srgb ? 1.0f : 0.0f;
    for (int i = 0; i < 4; ++i) { c.cscale[i] = l.colorScale[i]; c.cbias[i] = l.colorBias[i]; }
    if (!Upload(ctx, cb_.Get(), &c, sizeof(c))) return false;
    const D3D11_VIEWPORT vp{(float)E.x0, 0.0f, (float)E.w, (float)E.h, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtv = ringRtv_[slot].Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ID3D11ShaderResourceView* srv[2] = {atlasSrv_[l.family][slot].Get(), scratchSrv_.Get()};
    ctx->PSSetShaderResources(0, 2, srv);
    ctx->PSSetShader(ray ? psRay_.Get() : ps_.Get(), nullptr, 0);
    ctx->Draw((UINT)nv, 0);
    return true;
}

int OverlayCompositor::Draw(ID3D11DeviceContext* ctx, UINT slot, const OverlayEye eyes[2], const OverlayDraw* d, uint32_t n,
                            uint32_t nBehind, bool projUnpremul, const float* headCorr) {
    if (!ready_ || !ctx || slot >= ring_.size() || !d || !n) return 0;
    if (nBehind > n) nBehind = n;
    bound_ = false;
    int draws = 0;
    ID3D11ShaderResourceView* nullSrv[3] = {};
    for (int e = 0; e < 2; ++e) {
        const OverlayEye& E = eyes[e];
        if (!E.valid || !E.w || !E.h || E.x0 + E.w > ringW_ || E.h > ringH_) continue;
        // layers behind the projection layer: save the eye, clear it to black (what is below the first layer), draw
        // those layers, then blend the saved projection over them with its own alpha
        bool anyBehind = false;
        for (uint32_t k = 0; k < nBehind; ++k) anyBehind = anyBehind || ((d[k].l.eyeMask & (1u << e)) && HasFamily(d[k].l.family));
        ComPtr<ID3D11DeviceContext1> ctx1;
        if (anyBehind && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) &&
            EnsureTex(proj_, projSrv_, projW_, projH_, E.w, E.h, "projection copy") &&
            EnsureTex(scratch_, scratchSrv_, scratchW_, scratchH_, E.w, E.h, "destination scratch")) {
            Bind(ctx);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->PSSetShaderResources(0, 3, nullSrv);
            const D3D11_BOX eyeBox{E.x0, 0, 0, E.x0 + E.w, E.h, 1};
            ctx->CopySubresourceRegion(proj_.Get(), 0, 0, 0, 0, ring_[slot].Get(), 0, &eyeBox);
            const float black[4] = {0, 0, 0, 1};
            const D3D11_RECT rc{(LONG)E.x0, 0, (LONG)(E.x0 + E.w), (LONG)E.h};
            ctx1->ClearView(ringRtv_[slot].Get(), black, &rc, 1);
            for (uint32_t k = 0; k < nBehind; ++k) draws += DrawLayer(ctx, slot, E, e, d[k], k, headCorr) ? 1 : 0;
            // projection OVER the behind layers
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->PSSetShaderResources(0, 3, nullSrv);
            ctx->CopySubresourceRegion(scratch_.Get(), 0, 0, 0, 0, ring_[slot].Get(), 0, &eyeBox);
            OverlayVertex v[4];
            OverlayCb c{};
            EyeCb(c, E);
            c.dst[0] = (float)E.x0; c.dst[1] = 0.0f;
            c.flags[1] = projUnpremul ? 1.0f : 0.0f;
            c.flags[2] = ringSrgb_ ? 1.0f : 0.0f;
            if (Upload(ctx, vb_.Get(), v, sizeof(OverlayVertex) * (size_t)FullRect(v)) && Upload(ctx, cb_.Get(), &c, sizeof(c))) {
                const D3D11_VIEWPORT vp{(float)E.x0, 0.0f, (float)E.w, (float)E.h, 0.0f, 1.0f};
                ctx->RSSetViewports(1, &vp);
                ID3D11RenderTargetView* rtv = ringRtv_[slot].Get();
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                ID3D11ShaderResourceView* srv[3] = {nullptr, scratchSrv_.Get(), projSrv_.Get()};
                ctx->PSSetShaderResources(0, 3, srv);
                ctx->PSSetShader(psOver_.Get(), nullptr, 0);
                ctx->Draw(4, 0);
            }
        }
        for (uint32_t k = nBehind; k < n; ++k) draws += DrawLayer(ctx, slot, E, e, d[k], k, headCorr) ? 1 : 0;
    }
    if (bound_) {
        ID3D11Buffer* nullVb = nullptr;
        const UINT zero = 0;
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->PSSetShaderResources(0, 3, nullSrv);
        ctx->IASetVertexBuffers(0, 1, &nullVb, &zero, &zero);
        ctx->IASetInputLayout(nullptr);
        ctx->RSSetState(nullptr);
        ctx->VSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(nullptr, nullptr, 0);
        bound_ = false;
    }
    return draws;
}

void OverlayCompositor::Release() {
    ready_ = false;
    bound_ = false;
    ringRtv_.clear();
    ring_.clear();
    for (auto& a : atlasSrv_) a.clear();
    vs_.Reset(); ps_.Reset(); psRay_.Reset(); psOver_.Reset(); il_.Reset(); vb_.Reset(); cb_.Reset(); rs_.Reset();
    scratch_.Reset(); scratchSrv_.Reset(); proj_.Reset(); projSrv_.Reset();
    scratchW_ = scratchH_ = projW_ = projH_ = 0;
    dev_.Reset();
    loggedScratch_ = false;
    debugDraws_ = 24;
}

void OverlayPack(const OverlayFrame& ov, uint32_t readyMask, uint32_t failedMask, OverlayPacked& out, bool& loggedFull,
                 const char* who, const char* consequence) {
    out.nCopy = 0;
    out.nDraw = out.nBehind = 0;
    out.layersIn = out.notReady = out.full = out.setupFailed = 0;
    out.wantBits = 0;
    const uint32_t n = std::min<uint32_t>(ov.n, kMaxOverlays);
    out.layersIn = n;
    struct Shelf { UINT cx = 0, cy = 0, rowH = 0; } shelf[kOverlayFamilies];
    const UINT A = kOverlayAtlasSize;
    for (uint32_t i = 0; i < n; ++i) {
        const OverlayLayer& la = ov.layer[i];
        if (la.family < 0 || la.family >= kOverlayFamilies) continue;
        const uint32_t bit = 1u << la.family;
        if (failedMask & bit) { ++out.setupFailed; continue; }
        if (!(readyMask & bit)) { out.wantBits |= bit; ++out.notReady; continue; }
        const bool cube = la.kind == OverlayKind::Cube;
        const UINT bw = cube ? 3 * la.w : la.w, bh = cube ? 2 * la.h : la.h, nc = cube ? 6 : 1;
        OverlayDraw& d = out.draws[out.nDraw];
        d.l = la;
        bool placed = false;
        for (uint32_t j = 0; j < out.nDraw && !placed; ++j) {   // the same sub-image again (e.g. one per eye): reuse it
            const OverlayLayer& o = out.draws[j].l;
            if (o.image == la.image && o.arrayIndex == la.arrayIndex && o.x == la.x && o.y == la.y && o.w == la.w &&
                o.h == la.h && (o.kind == OverlayKind::Cube) == cube) {
                d.ax = out.draws[j].ax; d.ay = out.draws[j].ay; placed = true;
            }
        }
        if (!placed) {
            Shelf& s = shelf[la.family];
            if (s.cx + bw > A) { s.cx = 0; s.cy += s.rowH; s.rowH = 0; }
            if (bw > A || s.cy + bh > A || out.nCopy + nc > kMaxOverlayCopies) {
                ++out.full;
                if (!loggedFull) {
                    loggedFull = true;
                    LOGW("%s: overlay sub-image %ux%u (%s) does not fit the %ux%u %s overlay atlas next to this frame's "
                         "other overlays -- %s", who, bw, bh, OverlayKindName(la.kind), A, A, OverlayFamilyName(la.family),
                         consequence);
                }
                continue;
            }
            d.ax = s.cx; d.ay = s.cy;
            for (UINT f = 0; f < nc; ++f) {
                OverlayCopy& c = out.copies[out.nCopy++];
                c.image = la.image; c.sc = la.sc; c.scKey = la.scKey; c.family = la.family;
                c.arrayIndex = la.arrayIndex + f;
                c.x = la.x; c.y = la.y; c.w = la.w; c.h = la.h;
                c.dstX = s.cx + (f % 3) * la.w; c.dstY = s.cy + (f / 3) * la.h;
            }
            s.cx += bw;
            s.rowH = std::max(s.rowH, bh);
        }
        if (la.behind) ++out.nBehind;   // behind layers come first in ov.layer
        ++out.nDraw;
    }
}

void OverlayAtlasSet::MakePending(IFrameSource* src, ID3D11Device* dev, const std::vector<ComPtr<ID3D11Texture2D>>& ring,
                                  DXGI_FORMAT fmt, bool srgb, const char* who, const char* notWhat, const char* devName,
                                  const char* before) {
    const uint32_t pending = want.load() & ~(ready.load() | failed.load());
    if (!pending) return;
    // view / shader / texture creation is free-threaded: nothing touches the immediate context here
    if (!comp.Ready() && !comp.Init(dev, ring, fmt, srgb)) {
        failed.fetch_or((1u << kOverlayFamilies) - 1);
        LOGE("%s: overlay compositor setup failed -- overlays are not %s", who, notWhat);
        return;
    }
    const UINT A = kOverlayAtlasSize, count = (UINT)ring.size();
    for (int f = 0; f < kOverlayFamilies; ++f) {
        const uint32_t bit = 1u << f;
        if (!(pending & bit)) continue;
        // typed member of the family (D3D11 cannot open a TYPELESS D3D12 shared resource; the shader decodes sRGB itself)
        const DXGI_FORMAT typed = OverlayFamilyFormat(f);
        const bool ok = typed != DXGI_FORMAT_UNKNOWN && src->CreateOverlayAtlas(dev, A, A, typed, count, f, atlas_[f]) &&
                        comp.AddAtlas(f, atlas_[f], typed);
        if (!ok) {
            failed.fetch_or(bit);
            LOGE("%s: overlay atlas setup failed (%s) -- layers of that format family are not %s", who, DxgiFormatName(typed),
                 notWhat);
            continue;
        }
        const double mb = (double)A * A * OverlayFamilyBytes(f) * count / (1024.0 * 1024.0);
        LOGI("%s: overlays: %s atlas %ux%u %s x %u ring slots (+%.0f MB VRAM, shared with the game device) + compositor on "
             "the %s; blended in %s into the ring before %s", who, OverlayFamilyName(f), A, A, DxgiFormatName(typed), count,
             mb, devName, srgb ? "linear light (sRGB decode / encode in the shader)" : "linear light", before);
        ready.fetch_or(bit, std::memory_order_release);
    }
}

void OverlayAtlasSet::Release() {
    comp.Release();
    for (auto& a : atlas_) a.clear();
}
