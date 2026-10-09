// head_smoothing -- see head_smoothing.h.
#include "head_smoothing.h"
#include <algorithm>
#include <cmath>

namespace {

const double kPi = 3.14159265358979323846;
const double kDeg = kPi / 180.0;

struct V3 { double x, y, z; };
using Q = SmoothQuat;

Q Mul(const Q& a, const Q& b) {
    Q r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}
Q Conj(const Q& a) { Q r; r.x = -a.x; r.y = -a.y; r.z = -a.z; r.w = a.w; return r; }
double Dot(const Q& a, const Q& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
Q Normalize(const Q& a) {
    const double n = std::sqrt(Dot(a, a));
    if (n < 1e-12) return Q();
    Q r; r.x = a.x / n; r.y = a.y / n; r.z = a.z / n; r.w = a.w / n;
    return r;
}
// Rotation angle of a unit quaternion (0..pi).
double Angle(const Q& a) { return 2.0 * std::acos(std::min(1.0, std::fabs(a.w))); }
double AngleBetween(const Q& a, const Q& b) { return Angle(Mul(Conj(a), b)); }

Q Slerp(const Q& a, Q b, double t) {
    double d = Dot(a, b);
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
    Q r;
    if (d > 0.9995) {
        r.x = a.x + (b.x - a.x) * t; r.y = a.y + (b.y - a.y) * t; r.z = a.z + (b.z - a.z) * t; r.w = a.w + (b.w - a.w) * t;
        return Normalize(r);
    }
    const double th = std::acos(d), s = std::sin(th);
    const double wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    r.x = wa * a.x + wb * b.x; r.y = wa * a.y + wb * b.y; r.z = wa * a.z + wb * b.z; r.w = wa * a.w + wb * b.w;
    return Normalize(r);
}

// Row-major rotation matrix of a unit quaternion (v' = M v).
void ToMatrix(const Q& q, double m[9]) {
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz); m[1] = 2 * (xy - wz);     m[2] = 2 * (xz + wy);
    m[3] = 2 * (xy + wz);     m[4] = 1 - 2 * (xx + zz); m[5] = 2 * (yz - wx);
    m[6] = 2 * (xz - wy);     m[7] = 2 * (yz + wx);     m[8] = 1 - 2 * (xx + yy);
}

Q FromMatrix(const double m[9]) {
    Q q;
    const double tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q.w = 0.25 * s; q.x = (m[7] - m[5]) / s; q.y = (m[2] - m[6]) / s; q.z = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2;
        q.w = (m[7] - m[5]) / s; q.x = 0.25 * s; q.y = (m[1] + m[3]) / s; q.z = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2;
        q.w = (m[2] - m[6]) / s; q.x = (m[1] + m[3]) / s; q.y = 0.25 * s; q.z = (m[5] + m[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2;
        q.w = (m[3] - m[1]) / s; q.x = (m[2] + m[6]) / s; q.y = (m[5] + m[7]) / s; q.z = 0.25 * s;
    }
    return Normalize(q);
}

V3 Rotate(const double m[9], const V3& v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// Same forward direction, no roll: the camera's +X stays horizontal (perpendicular to the space's +Y).
Q RemoveRoll(const Q& q) {
    double m[9];
    ToMatrix(q, m);
    const V3 back{m[2], m[5], m[8]};                     // camera +Z (= -forward) in space
    if (std::fabs(back.y) > 0.995) return q;              // looking straight up / down: roll undefined
    // x = normalize(cross(up, z)), y = cross(z, x)
    V3 x{back.z, 0.0, -back.x};
    const double n = std::sqrt(x.x * x.x + x.z * x.z);
    x.x /= n; x.z /= n;
    const V3 y{back.y * x.z - back.z * x.y, back.z * x.x - back.x * x.z, back.x * x.y - back.y * x.x};
    const double r[9] = {x.x, y.x, back.x, x.y, y.y, back.y, x.z, y.z, back.z};   // columns x, y, z
    return FromMatrix(r);
}

double SmoothStep(double x) {
    x = std::min(1.0, std::max(0.0, x));
    return x * x * (3 - 2 * x);
}

// Does the output frustum (outTan, through C) lie inside the source frustum (srcTan)? The output frustum is the
// convex cone of its four corner rays and the source frustum is convex, so the corners decide.
bool Covered(const double c[9], const float outTan[4], const float srcTan[4]) {
    const double eps = 1e-5;
    for (int i = 0; i < 4; ++i) {
        const V3 d = Rotate(c, {(double)outTan[i & 1], (double)outTan[2 + (i >> 1)], -1.0});
        if (d.z > -1e-9) return false;
        const double x = d.x / -d.z, y = d.y / -d.z;
        if (x < srcTan[0] - eps || x > srcTan[1] + eps || y > srcTan[2] + eps || y < srcTan[3] - eps) return false;
    }
    return true;
}

void Identity(SmoothEyeOut& e) {
    const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) e.m[i] = id[i];
    e.outTan[0] = e.srcTan[0] = -1; e.outTan[1] = e.srcTan[1] = 1;
    e.outTan[2] = e.srcTan[2] = 1;  e.outTan[3] = e.srcTan[3] = -1;
    e.covered = true;
}

} // namespace

SymmetricFov ComputeSymmetricFov(const float fovL[4], const float fovR[4], uint32_t wL, uint32_t hL, uint32_t wR, uint32_t hR) {
    SymmetricFov r;
    const float* f[2] = {fovL, fovR};
    const uint32_t w[2] = {wL, wR}, h[2] = {hL, hR};
    double tanH = 1e9, tanV = 1e9, dH = 0, dV = 0;
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i)
            if (!std::isfinite(f[e][i]) || std::fabs(f[e][i]) >= 1.553f) return r;
        if (!(f[e][0] < f[e][1]) || !(f[e][3] < f[e][2]) || !w[e] || !h[e]) return r;
        const double l = std::tan((double)f[e][0]), rr = std::tan((double)f[e][1]);
        const double u = std::tan((double)f[e][2]), d = std::tan((double)f[e][3]);
        tanH = std::min(tanH, std::min(std::fabs(l), std::fabs(rr)));
        tanV = std::min(tanV, std::min(std::fabs(u), std::fabs(d)));
        dH = std::max(dH, w[e] / (rr - l));
        dV = std::max(dV, h[e] / (u - d));
    }
    // A view that does not contain its own centre line (both angles on one side) has no symmetric part.
    if (!(tanH > 1e-3) || !(tanV > 1e-3)) return r;
    r.tanH = tanH;
    r.tanV = tanV;
    r.eyeW = 2u * (uint32_t)std::lround(tanH * dH);   // round_even(2 tanH dH)
    r.eyeH = 2u * (uint32_t)std::lround(tanV * dV);
    r.valid = r.eyeW >= 32 && r.eyeH >= 32;
    return r;
}

double HeadSmoothingTimeConstant(float s) {
    if (s <= 0) return 0;
    return 0.05 * std::pow(30.0, ((double)s - 0.1) / 0.9);
}

void HeadSmoother::Configure(const HeadSmoothingSettings& s) {
    s_ = s;
    tau_ = std::max(1e-3, HeadSmoothingTimeConstant(s.smoothing));
    tauFast_ = std::min(tau_, 0.06);
    Reset();
}

void HeadSmoother::Update(int64_t ns, const SmoothEyeIn in[2], SmoothFrame& out) {
    out = SmoothFrame();
    Identity(out.eye[0]);
    Identity(out.eye[1]);
    if (!in[0].valid && !in[1].valid) { out.noPose = true; return; }

    // actual head orientation
    const Q qL = Normalize(in[0].valid ? in[0].q : in[1].q);
    const Q qR = Normalize(in[1].valid ? in[1].q : in[0].q);
    const Q H = Slerp(qL, qR, 0.5);

    // filter
    const double dt = have_ ? (double)(ns - lastNs_) * 1e-9 : 0.0;
    if (!s_.Enabled()) {
        S_ = H;                       // no filter: output = actual head (symmetric_fov only)
        have_ = true;
    } else if (!have_ || dt > 0.5) {
        S_ = H;
        catch_ = 0;
        have_ = true;
        out.reset = true;
    } else if (dt > 0) {
        const double e = AngleBetween(S_, H) / kDeg;
        const double f = std::max(0.1, (double)s_.followDeg);
        if (e > f) catch_ = std::min(1.0, catch_ + dt / 0.1);
        else if (e < 0.25 * f) catch_ = std::max(0.0, catch_ - dt / 0.3);
        const double tau = std::exp((1 - catch_) * std::log(tau_) + catch_ * std::log(tauFast_));
        double a = 1 - std::exp(-dt / tau);
        a += (1 - a) * SmoothStep((e - f) / f);
        S_ = Slerp(S_, H, a);
    }
    if (dt > 0 || out.reset) lastNs_ = ns;
    out.catchUp = catch_ > 0;

    Q O = s_.rollLock ? RemoveRoll(S_) : S_;

    // per eye: C_e = q_e^-1 * O * H^-1 * q_e  (symmetric output: parallel cameras, C_e = q_e^-1 * O)
    const double k = s_.Enabled() ? std::min(1.0, std::max(0.1, (double)s_.fovScale)) : 1.0;
    const bool sym = symH_ > 0 && symV_ > 0;
    const Q Hinv = Conj(H);
    auto build = [&](const Q& o, double cm[2][9]) {
        bool all = true;
        for (int e = 0; e < 2; ++e) {
            SmoothEyeOut& eo = out.eye[e];
            if (!in[e].valid) { Identity(eo); continue; }
            const Q qe = e == 0 ? qL : qR;
            ToMatrix(sym ? Mul(Conj(qe), o) : Mul(Mul(Conj(qe), o), Mul(Hinv, qe)), cm[e]);
            eo.srcTan[0] = (float)in[e].tanL; eo.srcTan[1] = (float)in[e].tanR;
            eo.srcTan[2] = (float)in[e].tanU; eo.srcTan[3] = (float)in[e].tanD;
            if (sym) {
                eo.outTan[0] = (float)(-k * symH_); eo.outTan[1] = (float)(k * symH_);
                eo.outTan[2] = (float)(k * symV_);  eo.outTan[3] = (float)(-k * symV_);
            } else {
                for (int i = 0; i < 4; ++i) eo.outTan[i] = (float)(k * eo.srcTan[i]);
            }
            eo.covered = Covered(cm[e], eo.outTan, eo.srcTan);
            all = all && eo.covered;
        }
        return all;
    };
    double cm[2][9];
    bool all = build(O, cm);
    if (!all && s_.edgeGuard && s_.Enabled()) {
        // smallest b with slerp(O, H, b) covered (b = 1 is the actual head: always covered since k <= 1)
        double lo = 0, hi = 1;
        for (int it = 0; it < 14; ++it) {
            const double mid = 0.5 * (lo + hi);
            double tmp[2][9];
            if (build(Slerp(O, H, mid), tmp)) hi = mid; else lo = mid;
        }
        O = Slerp(O, H, hi);
        all = build(O, cm);
        out.guarded = true;
    }
    for (int e = 0; e < 2; ++e)
        if (in[e].valid) for (int i = 0; i < 9; ++i) out.eye[e].m[i] = (float)cm[e][i];
    out.uncovered = !all;
    out.correctionDeg = AngleBetween(O, H) / kDeg;
}
