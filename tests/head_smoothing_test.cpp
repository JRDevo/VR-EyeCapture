// head_smoothing_test -- CPU unit test of src/head_smoothing (filter, conventions, roll lock, follow, edge guard) and
// src/dome (VR180 / VR360 size, lon / lat convention, dome frame, eye matrix, world lock).
// No GPU, no files. Exit code 0 = all checks passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "dome.h"
#include "head_smoothing.h"
#include "test_motion.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what, double v) {
    printf("  %s %s (%.4f)\n", ok ? "ok  " : "FAIL", what, v);
    if (!ok) ++g_fail;
}
SmoothQuat Q(const test_motion::Quat& q) { SmoothQuat r; r.x = q.x; r.y = q.y; r.z = q.z; r.w = q.w; return r; }
SmoothQuat YawPitchRoll(double y, double p, double r) {
    using namespace test_motion;
    return Q(Mul(Axis(0, 1, 0, y * kDeg), Mul(Axis(1, 0, 0, p * kDeg), Axis(0, 0, 1, r * kDeg))));
}
void Eyes(SmoothEyeIn in[2], const SmoothQuat& q) {
    for (int e = 0; e < 2; ++e) {
        in[e].valid = true;
        in[e].q = q;
        in[e].tanL = std::tan(e == 0 ? -0.87 : -0.75); in[e].tanR = std::tan(e == 0 ? 0.75 : 0.87);
        in[e].tanU = std::tan(0.80); in[e].tanD = std::tan(-0.85);
    }
}
// C applied to the output camera's forward ray -> direction in the actual eye camera
void Forward(const SmoothEyeOut& o, double d[3]) {
    d[0] = -o.m[2]; d[1] = -o.m[5]; d[2] = -o.m[8];
}
} // namespace

int main() {
    const int64_t dt = 11111111;   // 90 Hz in ns
    SmoothFrame f;
    SmoothEyeIn in[2];

    printf("time constants: 0.1 -> %.3f s, 0.6 -> %.3f s, 1.0 -> %.3f s\n", HeadSmoothingTimeConstant(0.1f),
           HeadSmoothingTimeConstant(0.6f), HeadSmoothingTimeConstant(1.0f));
    Check(std::fabs(HeadSmoothingTimeConstant(0.1f) - 0.05) < 1e-6 && std::fabs(HeadSmoothingTimeConstant(1.0f) - 1.5) < 1e-6,
          "mapping 0.1 -> 0.05 s, 1.0 -> 1.5 s", HeadSmoothingTimeConstant(0.6f));

    {   // first frame: reset to the actual pose, identity mapping (fov scale 1)
        HeadSmoothingSettings s; s.smoothing = 0.6f; s.fovScale = 1.0f;
        HeadSmoother hs(s);
        Eyes(in, YawPitchRoll(10, 5, 3));
        hs.Update(1000, in, f);
        double m = 0;
        for (int i = 0; i < 9; ++i) m = std::fmax(m, std::fabs(f.eye[0].m[i] - (i % 4 == 0 ? 1.0 : 0.0)));
        Check(f.reset && m < 1e-6 && f.eye[0].covered && f.correctionDeg < 1e-6, "first frame: reset, C = identity, covered", m);
    }
    {   // head turns LEFT 1 deg in one frame: the smoothed camera still looks (almost) where it did, i.e. to the RIGHT
        // of the actual camera -> output forward ray lands at x > 0 in the actual eye image; output x scale = k * src
        HeadSmoothingSettings s; s.smoothing = 0.6f; s.edgeGuard = false;
        HeadSmoother hs(s);
        Eyes(in, YawPitchRoll(0, 0, 0));
        hs.Update(0, in, f);
        Eyes(in, YawPitchRoll(1, 0, 0));
        hs.Update(dt, in, f);
        double d[3];
        Forward(f.eye[0], d);
        const double ang = std::atan2(d[0], -d[2]) / test_motion::kDeg;
        Check(ang > 0.9 && ang < 1.0, "turn left 1 deg -> correction samples ~0.97 deg to the right", ang);
        Check(std::fabs(f.eye[0].outTan[0] - 0.9 * std::tan(-0.87)) < 1e-6, "output fov = 0.9 x source tan", f.eye[0].outTan[0]);
        // pitch up 1 deg -> sample below (y < 0)
        HeadSmoother hp(s);
        Eyes(in, YawPitchRoll(0, 0, 0));
        hp.Update(0, in, f);
        Eyes(in, YawPitchRoll(0, 1, 0));
        hp.Update(dt, in, f);
        Forward(f.eye[0], d);
        const double up = std::atan2(d[1], -d[2]) / test_motion::kDeg;
        Check(up < -0.9 && up > -1.0, "pitch up 1 deg -> correction samples ~0.97 deg below", up);
    }
    {   // a gap > 0.5 s resets the filter
        HeadSmoothingSettings s; s.smoothing = 0.6f;
        HeadSmoother hs(s);
        Eyes(in, YawPitchRoll(0, 0, 0));
        hs.Update(0, in, f);
        Eyes(in, YawPitchRoll(3, 0, 0));
        hs.Update(600000000, in, f);
        Check(f.reset && f.correctionDeg < 1e-6, "gap of 0.6 s -> filter reset", f.correctionDeg);
    }
    {   // roll_lock: a static 10 deg head roll is removed completely (guard off), the forward direction is kept
        HeadSmoothingSettings s; s.smoothing = 0.6f; s.rollLock = true; s.edgeGuard = false;
        HeadSmoother hs(s);
        Eyes(in, YawPitchRoll(20, 10, 10));
        for (int i = 0; i < 10; ++i) hs.Update(i * dt, in, f);
        double d[3];
        Forward(f.eye[0], d);
        const double fwdErr = std::sqrt(d[0] * d[0] + d[1] * d[1]) / -d[2];
        // output camera +X axis expressed in the actual camera = first column of C
        const double rollDeg = std::atan2(f.eye[0].m[3], f.eye[0].m[0]) / test_motion::kDeg;
        Check(fwdErr < 1e-5, "roll_lock keeps the view direction", fwdErr);
        Check(std::fabs(f.correctionDeg - 10.0) < 0.6, "roll_lock removes the 10 deg roll (correction ~10 deg)", f.correctionDeg);
        Check(std::fabs(std::fabs(rollDeg) - 10.0) < 0.6, "output horizon rotated ~10 deg against the head roll", rollDeg);
    }
    {   // follow: 60 deg/s turn for 1 s, then stop. Lag stays bounded; settles fast after the stop.
        HeadSmoothingSettings s; s.smoothing = 0.6f; s.edgeGuard = false;
        HeadSmoother hs(s);
        double maxLag = 0, lagAfter = 0;
        for (int i = 0; i <= 270; ++i) {
            const double t = i / 90.0;
            const double yaw = t < 0.5 ? 0 : t < 1.5 ? 60 * (t - 0.5) : 60;
            Eyes(in, YawPitchRoll(yaw, 0, 0));
            hs.Update(i * dt, in, f);
            if (t < 1.5) maxLag = std::fmax(maxLag, f.correctionDeg);
            if (std::fabs(t - 2.0) < 1e-6) lagAfter = f.correctionDeg;
        }
        Check(maxLag < 2 * s.followDeg, "follow: lag during a 60 deg/s turn stays below 2 x head_follow_deg", maxLag);
        Check(lagAfter < 1.0, "follow: < 1 deg left 0.5 s after the turn stops", lagAfter);
    }
    {   // edge guard: a large lag would uncover the output; the guard keeps it covered
        HeadSmoothingSettings s; s.smoothing = 1.0f; s.followDeg = 60; s.fovScale = 0.9f;
        HeadSmoother hs(s);
        Eyes(in, YawPitchRoll(0, 0, 0));
        hs.Update(0, in, f);
        Eyes(in, YawPitchRoll(8, 0, 0));
        hs.Update(dt, in, f);
        Check(f.guarded && !f.uncovered && f.eye[0].covered && f.eye[1].covered, "edge guard: 8 deg jump stays covered", f.correctionDeg);
        HeadSmoothingSettings s2 = s; s2.edgeGuard = false;
        HeadSmoother hn(s2);
        Eyes(in, YawPitchRoll(0, 0, 0));
        hn.Update(0, in, f);
        Eyes(in, YawPitchRoll(8, 0, 0));
        hn.Update(dt, in, f);
        Check(!f.guarded && f.uncovered, "edge_guard = 0: same jump is reported uncovered", f.correctionDeg);
    }
    {   // symmetric fov: intersection of both eyes, output size at the source density, rounded to even
        const double D = test_motion::kDeg;
        const float fl[4] = {(float)(-50 * D), (float)(40 * D), (float)(45 * D), (float)(-45 * D)};
        const float fr[4] = {(float)(-40 * D), (float)(50 * D), (float)(45 * D), (float)(-45 * D)};
        const SymmetricFov sf = ComputeSymmetricFov(fl, fr, 6120, 6496, 6120, 6496);
        const double dens = 6120.0 / (std::tan(40 * D) + std::tan(50 * D));
        const uint32_t wantW = 2u * (uint32_t)std::lround(std::tan(40 * D) * dens);
        Check(sf.valid && std::fabs(std::atan(sf.tanH) / D - 40.0) < 1e-3 && std::fabs(std::atan(sf.tanV) / D - 45.0) < 1e-3,
              "symmetric fov of L[-50,+40] R[-40,+50] x +-45 = +-40 x +-45 deg", std::atan(sf.tanH) / D);
        printf("    output per eye %ux%u (source 6120x6496)\n", sf.eyeW, sf.eyeH);
        Check(sf.eyeW == wantW && sf.eyeW % 2 == 0 && sf.eyeH == 6496, "output size: 2 tanH x density, even; height kept", sf.eyeW);
        // smaller vertical fov on one eye and different densities: min of the angles, max of the densities
        const float fr2[4] = {(float)(-45 * D), (float)(45 * D), (float)(40 * D), (float)(-42 * D)};
        const SymmetricFov s2 = ComputeSymmetricFov(fl, fr2, 1000, 1001, 1200, 1000);
        const double dH = std::max(1000.0 / (std::tan(50 * D) + std::tan(40 * D)), 1200.0 / (2 * std::tan(45 * D)));
        const double dV = std::max(1001.0 / (2 * std::tan(45 * D)), 1000.0 / (std::tan(40 * D) + std::tan(42 * D)));
        Check(std::fabs(std::atan(s2.tanV) / D - 40.0) < 1e-3 && s2.eyeW == 2u * (uint32_t)std::lround(std::tan(40 * D) * dH) &&
              s2.eyeH == 2u * (uint32_t)std::lround(std::tan(40 * D) * dV) && s2.eyeW % 2 == 0 && s2.eyeH % 2 == 0,
              "mixed eyes: min angle, max density, even sizes", (double)s2.eyeH);
        // a view that lies entirely on one side of its centre line has no symmetric part
        const float bad[4] = {(float)(5 * D), (float)(40 * D), (float)(45 * D), (float)(-45 * D)};
        Check(!ComputeSymmetricFov(bad, fr, 100, 100, 100, 100).valid, "fov not containing the centre line -> invalid", 0);

        // smoothing off + symmetric output: parallel cameras, C = q_e^-1 * H, both eyes the same output frustum
        HeadSmoothingSettings s; s.smoothing = 0.0f;
        HeadSmoother hs(s);
        hs.SetSymmetricOutput(sf.tanH, sf.tanV);
        SmoothEyeIn in2[2];
        Eyes(in2, YawPitchRoll(10, 3, 2));
        for (int e = 0; e < 2; ++e) {
            const float* fv = e == 0 ? fl : fr;
            in2[e].tanL = std::tan((double)fv[0]); in2[e].tanR = std::tan((double)fv[1]);
            in2[e].tanU = std::tan((double)fv[2]); in2[e].tanD = std::tan((double)fv[3]);
        }
        hs.Update(0, in2, f);
        double m = 0;
        for (int i = 0; i < 9; ++i) m = std::fmax(m, std::fabs(f.eye[1].m[i] - (i % 4 == 0 ? 1.0 : 0.0)));
        const bool same = f.eye[0].outTan[0] == f.eye[1].outTan[0] && f.eye[0].outTan[2] == f.eye[1].outTan[2];
        Check(m < 1e-6 && same && std::fabs(f.eye[0].outTan[1] - sf.tanH) < 1e-6 && f.eye[0].covered && f.eye[1].covered &&
              !f.uncovered, "smoothing off: identity rotation, same symmetric output frustum (scale 1), covered", m);
        hs.Update(dt, in2, f);
        Check(!f.reset && f.correctionDeg < 1e-9, "smoothing off: no filtering, no correction", f.correctionDeg);
    }
    {   // edge guard with the symmetric output: the guard tests the symmetric frustum x scale against each eye's own
        // source fov. A lagging smoothed view (fast turn, follow off) must be pulled back until both eyes are covered.
        const double D = test_motion::kDeg;
        const float fl[4] = {(float)(-50 * D), (float)(40 * D), (float)(45 * D), (float)(-45 * D)};
        const float fr[4] = {(float)(-40 * D), (float)(50 * D), (float)(45 * D), (float)(-45 * D)};
        const SymmetricFov sf = ComputeSymmetricFov(fl, fr, 1408, 1440, 1408, 1440);
        int bad = 0, guarded = 0;
        for (int guard = 1; guard >= 0; --guard) {
            HeadSmoothingSettings s; s.smoothing = 0.6f; s.fovScale = 0.9f; s.edgeGuard = guard != 0;
            HeadSmoother hs(s);
            hs.SetSymmetricOutput(sf.tanH, sf.tanV);
            int unc = 0, grd = 0;
            for (int i = 0; i <= 180; ++i) {   // 30 deg turn in 0.5 s + jitter, 2 s
                const double t = i / 90.0;
                const double yaw = (t < 0.5 ? 0 : t < 1.0 ? 60 * (t - 0.5) : 30) + 2 * std::sin(2 * test_motion::kPi * 3.3 * t);
                SmoothEyeIn e2[2];
                Eyes(e2, YawPitchRoll(yaw, 2 * std::sin(2 * test_motion::kPi * 4.1 * t), 2 * std::sin(2 * test_motion::kPi * 4.7 * t)));
                for (int e = 0; e < 2; ++e) {
                    const float* fv = e == 0 ? fl : fr;
                    e2[e].tanL = std::tan((double)fv[0]); e2[e].tanR = std::tan((double)fv[1]);
                    e2[e].tanU = std::tan((double)fv[2]); e2[e].tanD = std::tan((double)fv[3]);
                }
                hs.Update(i * dt, e2, f);
                if (f.uncovered) ++unc;
                if (f.guarded) ++grd;
            }
            if (guard) { bad = unc; guarded = grd; }
            else Check(unc > 0, "symmetric + edge_guard = 0: the same motion uncovers the output", unc);
        }
        Check(bad == 0 && guarded > 0, "symmetric + edge guard: never uncovered, guard acted", guarded);
    }
    {   // no usable pose -> pass-through
        HeadSmoothingSettings s; s.smoothing = 0.6f;
        HeadSmoother hs(s);
        SmoothEyeIn none[2];
        hs.Update(0, none, f);
        Check(f.noPose && f.eye[0].m[0] == 1.0f && f.eye[0].outTan[1] == f.eye[0].srcTan[1], "no pose -> identity pass-through", 0);
    }
    // ---------------------------------------------------------------------------------------------------- dome
    {   // dome size: density from the source, rounding, clamping to an 8192 frame
        const double D = test_motion::kDeg;
        const float fl[4] = {(float)(-50 * D), (float)(40 * D), (float)(45 * D), (float)(-45 * D)};
        const float fr[4] = {(float)(-40 * D), (float)(50 * D), (float)(45 * D), (float)(-45 * D)};
        const double dens = 1408.0 / (std::tan(50 * D) + std::tan(40 * D));
        const DomeSize a = ComputeDomeSize(DomeProjection::Vr180, 0, true, fl, fr, 1408, 1408);
        printf("    mock 1408 px eyes: density %.1f px/rad; vr180 %ux%u per eye, frame %ux%u\n", a.density, a.eyeW, a.eyeH, a.frameW, a.frameH);
        Check(a.valid && std::fabs(a.density - dens) < 1e-3 && a.eyeW == 2u * (uint32_t)std::lround(dens * test_motion::kPi / 2) &&
              a.eyeH == a.eyeW && a.frameW == 2 * a.eyeW && a.frameH == a.eyeW && !a.clamped,
              "vr180: per-eye width = round_even(density * pi), square, SBS frame", (double)a.eyeW);
        const DomeSize b = ComputeDomeSize(DomeProjection::Vr360, 0, true, fl, fr, 1408, 1408);
        printf("    mock 1408 px eyes: vr360 %ux%u per eye, frame %ux%u\n", b.eyeW, b.eyeH, b.frameW, b.frameH);
        Check(b.eyeW % 4 == 0 && std::abs((double)b.eyeW - dens * 2 * test_motion::kPi) <= 2.0 && b.eyeH == b.eyeW / 2 &&
              b.frameW == b.eyeW && b.frameH == b.eyeW, "vr360: width = density * 2 pi (multiple of 4), 2:1 per eye, top-bottom frame",
              (double)b.eyeW);
        const DomeSize q = ComputeDomeSize(DomeProjection::Vr180, 0, true, fl, fr, 3072, 3072);
        const DomeSize q3 = ComputeDomeSize(DomeProjection::Vr360, 0, true, fl, fr, 3072, 3072);
        printf("    3072 px eyes (Quest-3-like): density %.1f -> vr180 wants %u, frame %ux%u; vr360 wants %u, frame %ux%u\n",
               q.density, q.wanted, q.frameW, q.frameH, q3.wanted, q3.frameW, q3.frameH);
        Check(q.clamped && q.eyeW == 4096 && q.frameW == 8192 && q.frameH == 4096 && q3.clamped && q3.eyeW == 8192 &&
              q3.frameW == 8192 && q3.frameH == 8192, "clamped to an 8192 frame (vr180 4096 per eye, vr360 8192)", (double)q.wanted);
        const DomeSize e1 = ComputeDomeSize(DomeProjection::Vr180, 2001, true, fl, fr, 1408, 1408);
        const DomeSize e2 = ComputeDomeSize(DomeProjection::Vr180, 6000, true, fl, fr, 1408, 1408);
        const DomeSize e3 = ComputeDomeSize(DomeProjection::Vr360, 3002, false, fl, fr, 1408, 1408);
        Check(e1.eyeW % 2 == 0 && std::abs((int)e1.eyeW - 2001) == 1 && e1.fromExplicit && e2.eyeW == 4096 && e2.clamped &&
              e3.eyeW % 4 == 0 && std::abs((int)e3.eyeW - 3002) <= 2, "explicit dome_width: rounded (even / x4) and clamped", (double)e1.eyeW);
        const DomeSize nf = ComputeDomeSize(DomeProjection::Vr180, 0, false, fl, fr, 1000, 1000);
        Check(nf.valid && nf.fallback && nf.eyeW == 2u * (uint32_t)std::lround(500.0 * test_motion::kPi / 2),
              "no usable fov: density fallback eye width / 2", (double)nf.eyeW);
    }
    {   // lon / lat -> dome ray convention
        double r[3];
        DomeRay(test_motion::kPi / 2, 0, r);
        const bool px = std::fabs(r[0] - 1) < 1e-12 && std::fabs(r[1]) < 1e-12 && std::fabs(r[2]) < 1e-12;
        DomeRay(0, test_motion::kPi / 2, r);
        const bool py = std::fabs(r[1] - 1) < 1e-12 && std::fabs(r[0]) < 1e-12 && std::fabs(r[2]) < 1e-12;
        DomeRay(0, 0, r);
        const bool mz = std::fabs(r[2] + 1) < 1e-12 && std::fabs(r[0]) < 1e-12 && std::fabs(r[1]) < 1e-12;
        Check(px && py && mz, "dome ray: lon = +90 -> +X, lat = +90 -> +Y, lon = 0 -> -Z", 0);
    }
    {   // dome frame: yaw only, from the head at record start
        const DomeFrame f = ComputeDomeFrame(YawPitchRoll(25, 30, 10), YawPitchRoll(25, 30, 10));
        double m[9];
        QuatToMatrix(f.D, m);
        Check(std::fabs(f.yawDeg - 25) < 1e-9 && std::fabs(m[4] - 1) < 1e-12 && !f.degenerate,
              "dome frame: head yaw 25 / pitch 30 / roll 10 -> D = yaw 25 only (up stays +Y)", f.yawDeg);
        const DomeFrame g = ComputeDomeFrame(YawPitchRoll(40, 89, 0), YawPitchRoll(40, 89, 0));
        Check(g.degenerate && std::fabs(g.yawDeg) < 1e-12, "looking straight up: degenerate, D = identity (-Z forward)", g.yawDeg);
        // left / right eye slerp: eyes canted +-5 degrees about the head's yaw 10 -> D yaw 10
        const DomeFrame c = ComputeDomeFrame(YawPitchRoll(15, 0, 0), YawPitchRoll(5, 0, 0));
        Check(std::fabs(c.yawDeg - 10) < 1e-9, "D from slerp(qLeft, qRight, 0.5)", c.yawDeg);
    }
    {   // eye matrix for a yawed head: D = start head (yaw 0); head now yawed 20 deg LEFT: the dome ray at lon = -20
        // (to the left) is the eye's forward; lon = 0 lies at +20 deg (to the right) in the eye image
        SmoothQuat D;
        double m[9], r[3];
        DomeEyeMatrix(D, YawPitchRoll(20, 0, 0), m);
        DomeRay(-20 * test_motion::kDeg, 0, r);
        const double ax = m[0] * r[0] + m[1] * r[1] + m[2] * r[2], ay = m[3] * r[0] + m[4] * r[1] + m[5] * r[2],
                     az = m[6] * r[0] + m[7] * r[1] + m[8] * r[2];
        DomeRay(0, 0, r);
        const double bx = m[0] * r[0] + m[1] * r[1] + m[2] * r[2], bz = m[6] * r[0] + m[7] * r[1] + m[8] * r[2];
        const double ang = std::atan2(bx, -bz) / test_motion::kDeg;
        Check(std::fabs(ax) < 1e-12 && std::fabs(ay) < 1e-12 && std::fabs(az + 1) < 1e-12 && std::fabs(ang - 20) < 1e-9,
              "eye matrix, head yaw +20 (left): lon -20 = eye forward, lon 0 at +20 deg in the eye", ang);
    }
    {   // world lock: a direction at infinity (world -Z, the test marker) maps to the SAME equirect pixel whatever the
        // head does, and every frame's eye matrix sends that pixel's ray to exactly where the eye sees the marker
        const SmoothQuat start = YawPitchRoll(7, 5, 3);
        const DomeFrame f = ComputeDomeFrame(start, start);
        const double marker[3] = {0, 0, -1};
        double u0 = 0, v0 = 0;
        const bool in = DomeDirToUv(DomeProjection::Vr180, f.D, marker, u0, v0);
        double worst = 0;
        for (int i = 0; i < 40; ++i) {
            const SmoothQuat head = YawPitchRoll(7 + 30 * std::sin(i * 0.37), 5 * std::cos(i * 0.5), 4 * std::sin(i * 0.9));
            double m[9], r[3], h[9];
            DomeEyeMatrix(f.D, head, m);
            DomeRay((u0 - 0.5) * test_motion::kPi, (0.5 - v0) * test_motion::kPi, r);
            const double a[3] = {m[0] * r[0] + m[1] * r[1] + m[2] * r[2], m[3] * r[0] + m[4] * r[1] + m[5] * r[2],
                                 m[6] * r[0] + m[7] * r[1] + m[8] * r[2]};
            QuatToMatrix(QuatConj(head), h);   // where the eye camera sees the marker: head^-1 * marker
            const double e[3] = {h[2] * -1, h[5] * -1, h[8] * -1};
            worst = std::fmax(worst, std::sqrt((a[0] - e[0]) * (a[0] - e[0]) + (a[1] - e[1]) * (a[1] - e[1]) + (a[2] - e[2]) * (a[2] - e[2])));
        }
        printf("    marker (world -Z) at equirect u %.6f v %.6f (lon %+.3f deg, lat %+.3f deg) with D yaw %.2f deg\n", u0, v0,
               (u0 - 0.5) * 180, (0.5 - v0) * 180, f.yawDeg);
        Check(in && std::fabs((u0 - 0.5) * 180 - 7) < 1e-9 && std::fabs(v0 - 0.5) < 1e-12 && worst < 1e-12,
              "world lock: marker pixel independent of the head pose (lon = +start yaw, lat 0)", worst);
    }

    printf("=== head_smoothing_test %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
