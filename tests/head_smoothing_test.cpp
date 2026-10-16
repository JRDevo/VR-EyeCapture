// head_smoothing_test -- CPU unit test of src/head_smoothing (filter, conventions, roll lock, follow, edge guard) and
// src/dome (VR180 / VR360 size, lon / lat convention, dome frame, eye matrix, world lock) and src/yt_layout
// (record_for_youtube frame sizes: PredictYtLayout == the tray's pp::PredictLayout, the codec / max_width limits, taps).
// No GPU, no files. Exit code 0 = all checks passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "dome.h"
#include "head_smoothing.h"
#include "test_motion.h"
#include "yt_layout.h"
#include "../tray/postprocess.h"   // pp::PredictLayout (header-only arithmetic): the YouTube layout must match it exactly

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

    {   // re-projection output at the recorded size (max_width / codec limit)
        const StageFit n0 = FitStageOutput(3384, 3384, 4630, 0, 8192);
        Check(!n0.scaled && n0.eyeW[0] == 3384 && n0.eyeW[1] == 3384 && n0.h == 4630 && n0.taps == 1,
              "fits already: native size, 1 tap", (double)n0.h);
        const StageFit a = FitStageOutput(3384, 3384, 4630, 3840, 8192);
        Check(a.scaled && a.eyeW[0] == 1920 && a.eyeW[1] == 1920 && a.h == 2626 && a.taps == 2,
              "6768 wide -> max_width 3840: 1920 | 1920 x 2626, 1.76x -> 2 x 2 taps", (double)a.h);
        const StageFit b = FitStageOutput(3384, 3384, 4630, 1280, 8192);
        Check(b.eyeW[0] + b.eyeW[1] <= 1280 && b.eyeW[0] == 640 && b.h == 876 && b.taps == 3,
              "max_width 1280: 640 | 640 x 876, 5.3x -> 3 x 3 taps", (double)b.taps);
        const StageFit c = FitStageOutput(9272 / 2, 9272 / 2, 5242, 4096, 4096);   // H.264 limit, no max_width (as recorder.cpp passes it)
        Check(c.scaled && c.eyeW[0] + c.eyeW[1] <= 4096 && c.h <= 4096 && c.eyeW[0] % 2 == 0 && c.h % 2 == 0,
              "codec limit 4096 alone also shrinks the render", (double)(c.eyeW[0] + c.eyeW[1]));
        const StageFit d = FitStageOutput(1408, 1300, 1440, 1001, 0);   // unequal eyes (raw layout), odd max
        Check(d.eyeW[0] + d.eyeW[1] <= 1001 && d.eyeW[0] % 2 == 0 && d.eyeW[1] % 2 == 0 && d.eyeW[0] > d.eyeW[1],
              "unequal eyes keep their ratio, sum <= max_width, even", (double)(d.eyeW[0] + d.eyeW[1]));
        const StageFit g = FitStageOutput(6120, 6120, 6496, 8192, 8192);   // in game (ATS on VDXR, raw layout): 1.494x
        Check(g.scaled && g.eyeW[0] == 4096 && g.eyeW[1] == 4096 && g.h == 4348 && g.taps == 2,
              "12240x6496 -> 8192x4348 (1.49x) -> 2 x 2 taps", (double)g.taps);
        const StageFit lo = FitStageOutput(3384, 3384, 4630, 6000, 8192);   // 1.13x: one tap is enough
        Check(lo.scaled && lo.eyeW[0] + lo.eyeW[1] <= 6000 && lo.taps == 1, "6768 -> max_width 6000 (1.13x) -> 1 tap", (double)lo.taps);
        const StageFit hi = FitStageOutput(3384, 3384, 4630, 5600, 8192);   // 1.21x: just above the single-tap limit
        Check(hi.scaled && hi.taps == 2, "6768 -> max_width 5600 (1.21x) -> 2 x 2 taps", (double)hi.taps);
        const StageFit t = FitStageOutput(8000, 8000, 8000, 256, 0);
        Check(t.taps == 4, "huge minification -> taps clamped to 4", (double)t.taps);
        const StageFit h = FitStageOutput(1000, 1000, 9000, 0, 4096);   // height-limited
        Check(h.scaled && h.h <= 4096 && h.eyeW[0] <= 456, "height limit scales the width too", (double)h.h);
    }

    printf("record_for_youtube layout (yt_layout.h)\n");
    {
        auto is = [](const YtLayout& l, int cw, int ch, int fw, int fh, int sw, int sh) {
            return l.cw == cw && l.ch == ch && l.fw == fw && l.fh == fh && l.sw == sw && l.sh == sh;
        };
        // Quest 3 (symmetric eye 4096x4630) -- the README / DEVELOPMENT numbers
        const YtLayout q169 = PredictYtLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::Wide169, kYtShortSide);
        Check(is(q169, 4096, 2304, 7680, 4320, 7680, 2160), "4096x4630 16:9 tb_half: crop 4096x2304 -> frame 7680x4320, eye 7680x2160", q169.fw);
        const YtLayout q43 = PredictYtLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::Std43, kYtShortSide);
        Check(is(q43, 4096, 3072, 5760, 4320, 5760, 2160), "4096x4630 4:3 tb_half: crop 4096x3072 -> frame 5760x4320, eye 5760x2160", q43.fw);
        const YtLayout qn = PredictYtLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::None, kYtShortSide);
        Check(is(qn, 4096, 4630, 4320, 4884, 4320, 2442), "4096x4630 none tb_half: no crop -> frame 4320x4884 (short side = the width), eye 4320x2442", qn.fh);
        const YtLayout qsh = PredictYtLayout(4096, 4630, Layout3d::SbsHalf, EyeCrop::Wide169, kYtShortSide);
        Check(is(qsh, 4096, 2304, 7680, 4320, 3840, 4320), "4096x4630 16:9 sbs_half: frame 7680x4320, eye 3840x4320", qsh.fw);
        const YtLayout qsf = PredictYtLayout(4096, 4630, Layout3d::SbsFull, EyeCrop::Wide169, kYtShortSide);
        Check(is(qsf, 4096, 2304, 8192, 2304, 4096, 2304), "4096x4630 16:9 sbs_full: long side 8192 caps it -> frame 8192x2304", qsf.fw);
        const YtLayout qtf = PredictYtLayout(4096, 4630, Layout3d::TbFull, EyeCrop::Wide169, kYtShortSide);
        Check(is(qtf, 4096, 2304, 4320, 4860, 4320, 2430), "4096x4630 16:9 tb_full: frame 4320x4860, eye 4320x2430", qtf.fh);
        const YtLayout wide = PredictYtLayout(4096, 2000, Layout3d::TbHalf, EyeCrop::Wide169, kYtShortSide);
        Check(wide.ch == 2000, "an eye already wider than 16:9 is not cropped", wide.ch);
        // 4:3pad: the 4:3 crop, black bars left and right up to 16:9 (eye units, before the scale); the picture in the middle
        const YtLayout qp = PredictYtLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::Pad43, kYtShortSide);
        Check(is(qp, 4096, 3072, 7680, 4320, 7680, 2160) && qp.pw == 5462 && qp.vw == 5760 && qp.vx >= 958 && qp.vx <= 962,
              "4096x4630 4:3pad tb_half: crop 4096x3072, pad 5462 -> frame 7680x4320, eye 7680x2160, picture 5760 at ~960", qp.vx);
        Check(qp.vx + qp.vw <= qp.sw && q43.vw == q43.sw && q43.vx == 0 && q43.pw == q43.cw && q169.vw == q169.sw && q169.vx == 0,
              "4:3pad picture inside the eye; 4:3 / 16:9: picture = eye, no bars", q43.vx);
        const YtLayout wp = PredictYtLayout(4096, 2000, Layout3d::TbHalf, EyeCrop::Pad43, kYtShortSide);
        Check(is(wp, wide.cw, wide.ch, wide.fw, wide.fh, wide.sw, wide.sh) && wp.pw == wp.cw && wp.vw == wp.sw && wp.vx == 0,
              "4:3pad, an eye already wider than 16:9: neither cropped nor padded (= 16:9's numbers)", wp.pw);
        const YtLayout qpsh = PredictYtLayout(4096, 4630, Layout3d::SbsHalf, EyeCrop::Pad43, kYtShortSide);
        Check(is(qpsh, 4096, 3072, 7680, 4320, 3840, 4320) && qpsh.vw == 2880 && qpsh.vx >= 478 && qpsh.vx <= 482,
              "4096x4630 4:3pad sbs_half: frame 7680x4320, eye 3840x4320, picture 2880 at ~480", qpsh.vx);
        // run_selftests.ps1 youtube cases (XR_EYE_CAPTURE_TEST_YT_SHORT)
        const YtLayout ta = PredictYtLayout(1920, 1080, Layout3d::TbHalf, EyeCrop::Wide169, 540);
        Check(is(ta, 1920, 1080, 960, 540, 960, 270), "selftest a: 1920x1080 tb_half short 540 -> 960x540, eye 960x270", ta.fw);
        const YtLayout tb = PredictYtLayout(1920, 1080, Layout3d::SbsHalf, EyeCrop::Wide169, 540);
        Check(is(tb, 1920, 1080, 960, 540, 480, 540), "selftest b: 1920x1080 sbs_half short 540 -> 960x540, eye 480x540", tb.fw);
        const YtLayout tc = PredictYtLayout(1200, 1600, Layout3d::TbHalf, EyeCrop::Std43, 450);
        Check(is(tc, 1200, 900, 600, 452, 600, 226), "selftest c: 1200x1600 4:3 tb_half short 450 -> crop 1200x900 -> 600x452, eye 600x226", tc.fh);
        const YtLayout te = PredictYtLayout(1200, 1600, Layout3d::TbHalf, EyeCrop::Pad43, 450);
        Check(is(te, 1200, 900, 800, 452, 800, 226) && te.pw == 1600 && te.vw == 600 && te.vx == 100,
              "selftest e: 1200x1600 4:3pad tb_half short 450 -> pad 1600x900 -> 800x452, eye 800x226, picture 600 at 100", te.fw);
        const YtLayout td = PredictYtLayout(1920, 1080, Layout3d::TbHalf, EyeCrop::Wide169, kYtShortSide);
        Check(is(td, 1920, 1080, 7680, 4320, 7680, 2160), "selftest d: 1920x1080 tb_half short 4320 -> 7680x4320", td.fw);
        // identical to the tray's 8K-step arithmetic over a sweep (eye sizes, layouts, crops, short sides incl. 0 = native)
        int mism = 0, n = 0, badPic = 0;
        const Layout3d lays[4] = {Layout3d::TbHalf, Layout3d::SbsHalf, Layout3d::SbsFull, Layout3d::TbFull};
        const EyeCrop crops[4] = {EyeCrop::Wide169, EyeCrop::Std43, EyeCrop::None, EyeCrop::Pad43};
        const int shorts[4] = {0, 4320, 540, 2160};
        for (int w = 640; w <= 6200; w += 389)
            for (int h = 480; h <= 6600; h += 431)
                for (Layout3d l : lays)
                    for (EyeCrop c : crops)
                        for (int sh : shorts) {
                            const YtLayout a = PredictYtLayout(w, h, l, c, sh);
                            const pp::LayoutSize b = pp::PredictLayout(w, h, l, c, sh);
                            ++n;
                            if (a.cw != b.cw || a.ch != b.ch || a.fw != b.fw || a.fh != b.fh || a.sw != b.sw || a.sh != b.sh ||
                                a.pw != b.pw || a.vw != b.vw || a.vx != b.vx)
                                ++mism;
                            // the picture inside the eye; unpadded crops: the whole eye
                            if (a.vx < 0 || a.vx + a.vw > a.sw || (c != EyeCrop::Pad43 && (a.pw != a.cw || a.vw != a.sw || a.vx != 0))) ++badPic;
                        }
        Check(mism == 0 && n > 1000, "PredictYtLayout == pp::PredictLayout (tray 8K step) on every case of the sweep (mismatches)", mism);
        Check(badPic == 0, "sweep: the picture (vw at vx) inside the eye; = the eye unless 4:3pad (bad cases)", badPic);
        // limits on top (recorder: VideoCodecMaxSide, max_width)
        const YtFit fn = FitYtLayout(q169, Layout3d::TbHalf, VideoCodecMaxSide(VideoCodec::Hevc), 0);
        Check(!fn.Scaled() && fn.fw == 7680 && fn.fh == 4320 && fn.sw == 7680 && fn.sh == 2160, "HEVC, no max_width: 7680x4320 as predicted", fn.fw);
        const YtFit f264 = FitYtLayout(q169, Layout3d::TbHalf, VideoCodecMaxSide(VideoCodec::H264), 0);
        Check(f264.byCodec && !f264.byWidth && f264.fw == 4096 && f264.fh == 2304 && f264.sw == 4096 && f264.sh == 1152,
              "H.264 limit 4096: 7680x4320 -> 4096x2304, eye 4096x1152", f264.fh);
        const YtFit fmw = FitYtLayout(q169, Layout3d::TbHalf, VideoCodecMaxSide(VideoCodec::Hevc), 3840);
        Check(fmw.byWidth && !fmw.byCodec && fmw.fw == 3840 && fmw.fh == 2160 && fmw.sw == 3840 && fmw.sh == 1080,
              "max_width 3840: 7680x4320 -> 3840x2160, eye 3840x1080", fmw.fw);
        const YtFit fboth = FitYtLayout(q169, Layout3d::TbHalf, VideoCodecMaxSide(VideoCodec::H264), 3840);
        Check(fboth.byWidth && fboth.byCodec && fboth.fw == 3840 && fboth.fh == 2160, "H.264 + max_width 3840: the smaller (3840x2160)", fboth.fw);
        const YtFit fbig = FitYtLayout(q169, Layout3d::TbHalf, VideoCodecMaxSide(VideoCodec::Hevc), 9000);
        Check(!fbig.Scaled() && fbig.fw == 7680, "max_width above the frame width: unchanged", fbig.fw);
        const YtFit fs = FitYtLayout(qsh, Layout3d::SbsHalf, VideoCodecMaxSide(VideoCodec::H264), 0);
        Check(fs.fw == 2 * fs.sw && fs.fh == fs.sh && fs.fw <= 4096 && fs.fh <= 4096 && fs.sw % 2 == 0 && fs.sh % 2 == 0,
              "sbs_half under the H.264 limit: 2 eyes side by side, even, <= 4096", fs.fw);
        Check(fn.vw == fn.sw && fn.vx == 0 && f264.vw == f264.sw && f264.vx == 0, "16:9 under the limits: picture = the eye", f264.vw);
        const YtFit fp = FitYtLayout(qp, Layout3d::TbHalf, VideoCodecMaxSide(VideoCodec::Hevc), 0);
        Check(!fp.Scaled() && fp.fw == 7680 && fp.fh == 4320 && fp.vw == qp.vw && fp.vx == qp.vx, "4:3pad, HEVC: 7680x4320, picture as predicted", fp.vw);
        const YtFit fp264 = FitYtLayout(qp, Layout3d::TbHalf, VideoCodecMaxSide(VideoCodec::H264), 0);
        Check(fp264.fw == 4096 && fp264.fh == 2304 && fp264.vw == 3072 && fp264.vx >= 510 && fp264.vx <= 514 &&
              fp264.vx + fp264.vw <= fp264.sw && fp264.vx % 2 == 0,
              "4:3pad, H.264 4096: 4096x2304, picture 3072 at ~512 (even, inside the eye)", fp264.vx);
        const YtFit fodd = FitYtLayout(tc, Layout3d::TbHalf, 8192, 333);
        Check(fodd.fw <= 333 && fodd.sw % 2 == 0 && fodd.sh % 2 == 0 && fodd.fh == 2 * fodd.sh, "odd max_width: frame <= it, even eyes", fodd.fw);
        // taps: the larger minification of the two axes, FitStageOutput's rule
        Check(YtTaps(4096, 2304, 7680, 2160) == 1, "Quest 8K tb_half (0.53x wide, 1.07x tall): 1 tap", YtTaps(4096, 2304, 7680, 2160));
        Check(YtTaps(4096, 3072, 5760, 2160) == 2, "Quest 8K 4:3pad (picture 5760: 0.71x wide, 1.42x tall): 2 x 2 taps",
              YtTaps(4096, 3072, 5760, 2160));
        Check(YtTaps(1920, 1080, 960, 270) == 2, "selftest a (2x wide, 4x tall): 2 x 2 taps", YtTaps(1920, 1080, 960, 270));
        Check(YtTaps(1200, 900, 600, 226) == 2, "selftest c (2x, 3.98x): 2 x 2 taps", YtTaps(1200, 900, 600, 226));
        Check(YtTaps(4096, 2304, 3840, 1080) == 2, "max_width 3840 tb_half (1.07x, 2.13x): 2 x 2 taps", YtTaps(4096, 2304, 3840, 1080));
        Check(YtTaps(4096, 4096, 512, 256) == 4, "16x: clamped to 4", YtTaps(4096, 4096, 512, 256));
        Check(std::string(YtPackingKey(Layout3d::SbsFull)) == "sbs_full" && std::string(YtCropKey(EyeCrop::Std43)) == "4:3" &&
              std::string(YtCropKey(EyeCrop::Pad43)) == "4:3pad" && std::string(YtCropKey(EyeCrop::None)) == "none" &&
              YtTopBottom(Layout3d::TbFull) && !YtTopBottom(Layout3d::SbsHalf), "keys / top-bottom flag", 0);
    }

    printf("=== head_smoothing_test %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
