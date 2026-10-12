// overlay_test -- CPU part of record_overlays (src/overlay.h): the layer mesh projected into an eye's clip space, the
// pixel box, the winding that decides which side is visible, format families. No GPU.
// Setup = tests/layer_e2e --overlays on the mock runtime: eyes at (-+0.032, 1.6, 0), identity orientation, fov +-0.8
// rad, 1920 x 1080 per eye, right eye at x0 = 1920 of the side-by-side frame.
// Head smoothing + a head-locked (VIEW-space) quad: tests/test_motion.h jitter + turn, asymmetric fovs, flat and
// symmetric output, eyes parallel or canted: the quad's corner must stay at one output pixel (OverlayHeadLock).
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include "head_smoothing.h"
#include "overlay.h"
#include "test_motion.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
OverlayEye Eye(int e) {
    OverlayEye E;
    E.valid = true;
    E.p[0] = e == 0 ? -0.032f : 0.032f; E.p[1] = 1.6f; E.p[2] = 0.0f;
    E.fov[0] = -0.8f; E.fov[1] = 0.8f; E.fov[2] = 0.8f; E.fov[3] = -0.8f;
    E.x0 = e == 0 ? 0 : 1920; E.w = 1920; E.h = 1080;
    return E;
}
// pixel of a projection-space point in eye e (the same formula as run_e2e.ps1)
void Px(double x, double y, double z, int e, double& px, double& py) {
    const double T = std::tan(0.8), ex = e == 0 ? -0.032 : 0.032;
    px = ((x - ex) / -z + T) / (2 * T) * 1920 + 1920 * e;
    py = (T - (y - 1.6) / -z) / (2 * T) * 1080;
}
void Screen(const OverlayVertex& v, const OverlayEye& E, double& x, double& y) {
    x = E.x0 + (0.5 + 0.5 * v.x / v.w) * E.w;
    y = (0.5 - 0.5 * v.y / v.w) * E.h;
}
// signed area of the first strip triangle in screen space (y down): > 0 = clockwise = front face for D3D's default
double Winding(const OverlayVertex* v, const OverlayEye& E) {
    double x0, y0, x1, y1, x2, y2;
    Screen(v[0], E, x0, y0); Screen(v[1], E, x1, y1); Screen(v[2], E, x2, y2);
    return (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
}
// Head smoothing + a head-locked quad, the recorder's path on the CPU: the layer is placed on the submitted head like
// CollectOverlays does (head = nlerp of the eye orientations, midpoint of the eye positions; pose T in the VIEW space),
// moved by OverlayHeadLock with the frame's headCorr, projected into each ACTUAL eye (OverlayClipVertices), and that
// source ray is taken through the re-projection (output ray d = C^T a) to the output camera's tangent. Returns the
// largest drift of the quad's corner over 6 s of jitter + a 30 degree turn, in output pixels (~700 px per unit tan,
// the mock's density), with (lock = true) and without the correction.
double HeadLockDrift(bool sym, double cantDeg, bool lock) {
    using namespace test_motion;
    HeadSmoothingSettings hs;
    hs.smoothing = 0.6f; hs.fovScale = 0.9f;
    HeadSmoother sm(hs);
    if (sym) {
        const SymmetricFov f = ComputeSymmetricFov(kFovLeft, kFovRight, 1408, 1440, 1408, 1440);
        sm.SetSymmetricOutput(f.tanH, f.tanV);
    }
    const double kPxPerTan = 700.0;
    double ref[2][2] = {}, worst = 0;
    for (int f = 0; f < 540; ++f) {
        const double t = f / 90.0;
        const Quat H = Head(t);
        float m[9];
        Matrix(H, m);
        OverlayEye E[2];
        SmoothEyeIn in[2];
        for (int e = 0; e < 2; ++e) {
            const Quat q = Mul(H, Axis(0, 1, 0, (e == 0 ? cantDeg : -cantDeg) * kDeg));   // canted outwards
            const double ipd = e == 0 ? -0.032 : 0.032;
            E[e].valid = true;
            E[e].q[0] = (float)q.x; E[e].q[1] = (float)q.y; E[e].q[2] = (float)q.z; E[e].q[3] = (float)q.w;
            E[e].p[0] = (float)(m[0] * ipd); E[e].p[1] = (float)(1.6 + m[3] * ipd); E[e].p[2] = (float)(m[6] * ipd);
            for (int i = 0; i < 4; ++i) E[e].fov[i] = e == 0 ? kFovLeft[i] : kFovRight[i];
            E[e].x0 = e == 0 ? 0 : 1408; E[e].w = 1408; E[e].h = 1440;
            in[e].valid = true;
            in[e].q.x = q.x; in[e].q.y = q.y; in[e].q.z = q.z; in[e].q.w = q.w;
            in[e].tanL = std::tan((double)E[e].fov[0]); in[e].tanR = std::tan((double)E[e].fov[1]);
            in[e].tanU = std::tan((double)E[e].fov[2]); in[e].tanD = std::tan((double)E[e].fov[3]);
        }
        // the layer on the submitted head (as CollectOverlays places it): VIEW pose T = (0.12, -0.10, -1), 0.10 x 0.06 m
        double hq[4], n = 0;
        for (int i = 0; i < 4; ++i) { hq[i] = (double)E[0].q[i] + E[1].q[i]; n += hq[i] * hq[i]; }
        n = std::sqrt(n);
        for (double& c : hq) c /= n;
        float hm[9];
        Matrix({hq[0], hq[1], hq[2], hq[3]}, hm);
        const double T[3] = {0.12, -0.10, -1.0};
        OverlayLayer l;
        l.headLocked = true;
        l.width = 0.10f; l.height = 0.06f;
        for (int i = 0; i < 4; ++i) l.q[i] = (float)hq[i];
        for (int r = 0; r < 3; ++r)
            l.p[r] = (float)(0.5 * (E[0].p[r] + E[1].p[r]) + hm[3 * r] * T[0] + hm[3 * r + 1] * T[1] + hm[3 * r + 2] * T[2]);
        SmoothFrame sf;
        sm.Update((int64_t)f * 11111111, in, sf);
        const float corr[4] = {(float)sf.headCorr.x, (float)sf.headCorr.y, (float)sf.headCorr.z, (float)sf.headCorr.w};
        for (int e = 0; e < 2; ++e) {
            OverlayLayer le = l;
            if (lock) OverlayHeadLock(le, E[e], corr);
            OverlayVertex v[kOverlayMaxVertices];
            if (OverlayClipVertices(le, E[e], v, kOverlayMaxVertices) != 4 || v[0].w <= 0) return 1e9;
            const double tl = in[e].tanL, tr = in[e].tanR, tu = in[e].tanU, td = in[e].tanD;
            const double a[3] = {((v[0].x / v[0].w) * (tr - tl) + tr + tl) / 2, ((v[0].y / v[0].w) * (tu - td) + tu + td) / 2, -1.0};
            const float* C = sf.eye[e].m;   // actual-eye ray a = C * output ray
            double d[3];
            for (int i = 0; i < 3; ++i) d[i] = C[i] * a[0] + C[3 + i] * a[1] + C[6 + i] * a[2];
            const double ox = d[0] / -d[2] * kPxPerTan, oy = d[1] / -d[2] * kPxPerTan;
            if (f == 0) { ref[e][0] = ox; ref[e][1] = oy; }   // first frame: filter reset, O = H = no smoothing
            worst = std::max(worst, std::max(std::fabs(ox - ref[e][0]), std::fabs(oy - ref[e][1])));
        }
    }
    return worst;
}
} // namespace

int main() {
    printf("=== overlay_test\n");
    OverlayVertex v[kOverlayMaxVertices];
    for (int e = 0; e < 2; ++e) {
        const OverlayEye E = Eye(e);
        printf("eye %d\n", e);
        // A: quad in the projection space at (0, 1.45, -2), 0.5 x 0.5 m, front face +Z (towards the viewer)
        OverlayLayer a;
        a.p[0] = 0.0f; a.p[1] = 1.45f; a.p[2] = -2.0f; a.width = 0.5f; a.height = 0.5f;
        const int n = OverlayClipVertices(a, E, v, kOverlayMaxVertices);
        Check(n == 4, "quad: 4 vertices");
        double cx = 0, cy = 0;
        for (int i = 0; i < n; ++i) { double x, y; Screen(v[i], E, x, y); cx += x / n; cy += y / n; }
        double ex, ey;
        Px(0.0, 1.45, -2.0, e, ex, ey);
        printf("    quad centre on screen %.2f, %.2f (expected %.2f, %.2f), winding %.1f\n", cx, cy, ex, ey, Winding(v, E));
        Check(std::fabs(cx - ex) < 0.5 && std::fabs(cy - ey) < 0.5, "quad centre lands where the pinhole model puts it");
        Check(Winding(v, E) > 0, "quad seen from its front: clockwise (drawn, CullMode BACK)");
        double bx, by;
        Px(-0.25, 1.70, -2.0, e, bx, by);   // top-left corner
        double tx, ty;
        Screen(v[1], E, tx, ty);
        Check(std::fabs(tx - bx) < 0.5 && std::fabs(ty - by) < 0.5 && v[1].u == 0 && v[1].v == 0, "vertex 1 = top-left, uv (0, 0)");
        UINT x0, y0, x1, y1;
        Check(OverlayPixelBox(v, n, E, x0, y0, x1, y1) && x0 + 0.0 <= cx && cx <= x1 && y0 <= cy && cy <= y1 && x0 >= E.x0 &&
                  x1 <= E.x0 + E.w, "pixel box contains the quad, inside the eye's rectangle");
        printf("    box %u,%u .. %u,%u\n", x0, y0, x1, y1);
        // D: the same quad turned 180 degrees about +Y: its back faces the viewer
        OverlayLayer d = a;
        d.q[0] = 0; d.q[1] = 1; d.q[2] = 0; d.q[3] = 0;
        OverlayClipVertices(d, E, v, kOverlayMaxVertices);
        Check(Winding(v, E) < 0, "quad seen from behind: counter-clockwise (culled)");
        // C: cylinder in a VIEW-like frame: centre (0.5, 2.5, 0), radius 1.5, 0.4 rad, aspect 4, the viewer inside
        OverlayLayer c;
        c.kind = OverlayKind::Cylinder;
        c.p[0] = 0.5f; c.p[1] = 2.5f; c.p[2] = 0.0f; c.radius = 1.5f; c.centralAngle = 0.4f; c.aspect = 4.0f;
        const int nc = OverlayClipVertices(c, E, v, kOverlayMaxVertices);
        printf("    cylinder: %d vertices, winding %.1f\n", nc, Winding(v, E));
        Check(nc >= 6 && nc % 2 == 0, "cylinder: a strip of column pairs");
        Check(Winding(v, E) > 0, "cylinder seen from inside: clockwise (drawn)");
        // the middle column is the arc centre (0.5, 2.5 +- h/2, -1.5): average of its two vertices
        double mx1, my1, mx2, my2;
        Screen(v[nc / 2 - 1], E, mx1, my1);
        Screen(v[nc / 2], E, mx2, my2);
        double ccx, ccy;
        Px(0.5, 2.5, -1.5, e, ccx, ccy);
        printf("    cylinder arc centre on screen %.2f, %.2f (expected %.2f, %.2f)\n", 0.5 * (mx1 + mx2), 0.5 * (my1 + my2), ccx, ccy);
        Check(std::fabs(0.5 * (mx1 + mx2) - ccx) < 1.0 && std::fabs(0.5 * (my1 + my2) - ccy) < 1.0, "cylinder arc centre where expected");
        // a viewer OUTSIDE the cylinder looking at the arc's convex (outer) side: culled. Axis at z = -3.5, turned 180
        // degrees about +Y, so the arc sits at z = -2 between the viewer and the axis.
        OverlayLayer co = c;
        co.q[0] = 0; co.q[1] = 1; co.q[2] = 0; co.q[3] = 0;
        co.p[2] = -3.5f;
        OverlayClipVertices(co, E, v, kOverlayMaxVertices);
        Check(Winding(v, E) < 0, "cylinder seen from outside (convex side): counter-clockwise (culled)");
    }
    // a quad behind the eye: everything behind the near plane -> pixel box = the whole eye (the hardware clips)
    {
        const OverlayEye E = Eye(0);
        OverlayLayer b;
        b.p[0] = 0.0f; b.p[1] = 1.6f; b.p[2] = 2.0f; b.width = 0.5f; b.height = 0.5f;
        const int n = OverlayClipVertices(b, E, v, kOverlayMaxVertices);
        UINT x0, y0, x1, y1;
        Check(n == 4 && v[0].w < 0 && OverlayPixelBox(v, n, E, x0, y0, x1, y1) && x0 == 0 && x1 == 1920 && y1 == 1080,
              "quad behind the eye: w < 0, box = the whole eye (clipped by the hardware)");
    }
    // bad parameters -> nothing
    {
        OverlayLayer z;
        z.width = 0; z.height = 1;
        Check(OverlayClipVertices(z, Eye(0), v, kOverlayMaxVertices) == 0, "zero-size quad: no vertices");
        OverlayEye inv = Eye(0);
        inv.valid = false;
        z.width = 1;
        Check(OverlayClipVertices(z, inv, v, kOverlayMaxVertices) == 0, "eye without a usable pose: no vertices");
    }
    // ---- equirect / cube: the per-pixel ray lookup (CPU mirror of the pixel shader) ----
    {
        printf("ray layers\n");
        const OverlayEye E = Eye(0);
        double u = 0, vv = 0;
        int face = -1;
        // the eye's centre pixel looks along -Z (symmetric fov, identity orientation)
        OverlayLayer q;
        q.kind = OverlayKind::Equirect2;
        q.centralHorizontalAngle = 1.0f; q.upperVerticalAngle = 0.3f; q.lowerVerticalAngle = -0.3f;
        Check(OverlayClipVertices(q, E, v, kOverlayMaxVertices) == 0, "equirect: no mesh (drawn per pixel)");
        bool ok = OverlayRayLookup(q, E, 960, 540, u, vv, face);
        printf("    equirect2 centre: u %.4f v %.4f\n", u, vv);
        Check(ok && std::fabs(u - 0.5) < 0.002 && std::fabs(vv - 0.5) < 0.002, "equirect2: view centre -> image centre");
        ok = OverlayRayLookup(q, E, 1100, 540, u, vv, face);
        const double az = std::atan((1100.0 / 1920 * 2 - 1) * std::tan(0.8));
        Check(ok && std::fabs(u - (0.5 + az / 1.0)) < 0.002, "equirect2: right of centre -> u = 0.5 + azimuth / central angle (not mirrored)");
        ok = OverlayRayLookup(q, E, 960, 400, u, vv, face);
        const double el = std::atan((1 - 400.0 / 1080 * 2) * std::tan(0.8));
        Check(ok && std::fabs(vv - (0.3 - el) / 0.6) < 0.002, "equirect2: above centre -> v = (upper - elevation) / (upper - lower)");
        Check(!OverlayRayLookup(q, E, 960, 100, u, vv, face), "equirect2: above upperVerticalAngle -> not covered");
        Check(!OverlayRayLookup(q, E, 5, 540, u, vv, face), "equirect2: outside centralHorizontalAngle -> not covered");
        // the same layer turned 90 degrees about +Y: the view centre (-Z) is at azimuth +90 degrees of the layer -> uncovered
        OverlayLayer qr = q;
        qr.q[1] = 0.70710678f; qr.q[3] = 0.70710678f;
        Check(!OverlayRayLookup(qr, E, 960, 540, u, vv, face), "equirect2 turned 90 degrees: view centre not covered");
        // v1: full sphere, scale (1, 1), bias (0, 0)
        OverlayLayer e1;
        e1.kind = OverlayKind::Equirect;
        ok = OverlayRayLookup(e1, E, 960, 540, u, vv, face);
        Check(ok && std::fabs(u - 0.5) < 0.002 && std::fabs(vv - 0.5) < 0.002, "equirect v1: view centre -> (0.5, 0.5)");
        // v1 on a sphere of radius 2 centred at the head (0, 1.6, 0): the left eye (x -0.032) looks at (-0.032, 1.6, -1.99974)
        e1.radius = 2.0f;
        e1.p[1] = 1.6f;
        ok = OverlayRayLookup(e1, E, 960, 540, u, vv, face);
        const double azr = std::atan2(-0.032, std::sqrt(4.0 - 0.032 * 0.032));
        printf("    equirect v1 radius 2: u %.5f (expected %.5f)\n", u, azr / (2 * 3.14159265358979) + 0.5);
        Check(ok && std::fabs(u - (azr / (2 * 3.14159265358979) + 0.5)) < 2e-4, "equirect v1 finite radius: parallax of the eye offset");
        e1.radius = std::numeric_limits<float>::infinity();
        ok = OverlayRayLookup(e1, E, 960, 540, u, vv, face);
        Check(ok && std::fabs(u - 0.5) < 2e-4, "equirect v1 radius +inf = infinite (no parallax)");
        // v1 scale / bias: uv outside 0..1 -> not covered
        e1.radius = 0; e1.eqScale[0] = 8; e1.eqBias[0] = -3.5f;   // u = 8 * 0.5 - 3.5 = 0.5 at the centre
        ok = OverlayRayLookup(e1, E, 960, 540, u, vv, face);
        Check(ok && std::fabs(u - 0.5) < 0.002, "equirect v1 scale / bias");
        Check(!OverlayRayLookup(e1, E, 1900, 540, u, vv, face), "equirect v1: uv > 1 -> not covered");
        // cube: TextureCube face rules (major axis; -Z face: s = -x, t = -y)
        OverlayLayer c;
        c.kind = OverlayKind::Cube;
        ok = OverlayRayLookup(c, E, 960, 540, u, vv, face);
        Check(ok && face == 5 && std::fabs(u - 0.5) < 0.002 && std::fabs(vv - 0.5) < 0.002, "cube: view centre -> face 5 (-Z) centre");
        ok = OverlayRayLookup(c, E, 1100, 400, u, vv, face);
        Check(ok && face == 5 && u < 0.5 && vv < 0.5, "cube -Z face: right / up in the view -> smaller u / v (TextureCube rule s = -x, t = -y)");
        ok = OverlayRayLookup(c, E, 1915, 540, u, vv, face);
        Check(ok && face == 0, "cube: right edge (tan > 1) -> face 0 (+X)");
        ok = OverlayRayLookup(c, E, 960, 2, u, vv, face);
        Check(ok && face == 2, "cube: top edge -> face 2 (+Y)");
        c.q[1] = 0.70710678f; c.q[3] = 0.70710678f;   // turned 90 degrees about +Y: world -Z = the layer's +X
        ok = OverlayRayLookup(c, E, 960, 540, u, vv, face);
        Check(ok && face == 0 && std::fabs(u - 0.5) < 0.002, "cube turned 90 degrees about +Y: view centre -> face 0 (+X)");
    }
    Check(OverlayFamily(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == OverlayFamily(DXGI_FORMAT_R8G8B8A8_UNORM) &&
              OverlayFamily(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) != OverlayFamily(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
              OverlayFamily(DXGI_FORMAT_R10G10B10A2_UNORM) >= 0 && OverlayFamily(DXGI_FORMAT_R16G16B16A16_FLOAT) >= 0 &&
              OverlayFamily(DXGI_FORMAT_R11G11B10_FLOAT) >= 0 && OverlayFamily(DXGI_FORMAT_D32_FLOAT) < 0 &&
              OverlayFamilyFormat(OverlayFamily(DXGI_FORMAT_R16G16B16A16_TYPELESS)) == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "atlas families: sRGB + UNORM 8-bit share one, BGRA / 10-bit / FP16 / R11G11B10 have their own");
    Check(TypelessForDxgi(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
              TypelessForDxgi(DXGI_FORMAT_B8G8R8A8_TYPELESS) == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
              TypelessForDxgi(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_TYPELESS &&
              TypelessForDxgi(DXGI_FORMAT_D32_FLOAT) == DXGI_FORMAT_UNKNOWN,
          "format families");
    printf("head smoothing 0.6 (fov scale 0.9) + a head-locked (VIEW-space) quad, 6 s of jitter + a 30 degree turn\n");
    for (int sym = 0; sym < 2; ++sym) {
        for (double cant : {0.0, 2.0}) {   // (5 deg cant + symmetric output: never covered, the edge guard pins O = H)
            const double on = HeadLockDrift(sym != 0, cant, true), off = HeadLockDrift(sym != 0, cant, false);
            char what[220];
            snprintf(what, sizeof(what), "%s output, eyes canted %.0f deg: corner drift %.4f px with OverlayHeadLock (< 0.05), "
                     "%.1f px without (> 5: the shake it removes)", sym ? "symmetric" : "flat", cant, on, off);
            Check(on < 0.05 && off > 5, what);
        }
    }
    printf("=== overlay_test %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
