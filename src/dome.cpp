// dome -- see dome.h.
#include "dome.h"
#include <algorithm>
#include <cmath>

namespace {
const double kPi = 3.14159265358979323846;
}

const char* DomeProjectionName(DomeProjection p) {
    switch (p) {
    case DomeProjection::Vr180: return "vr180";
    case DomeProjection::Vr360: return "vr360";
    default:                    return "flat";
    }
}

const char* DomeFileTag(DomeProjection p) {
    switch (p) {
    case DomeProjection::Vr180: return "VR180";
    case DomeProjection::Vr360: return "VR360";
    default:                    return "SBS";
    }
}

double DomeLonSpan(DomeProjection p) {
    return p == DomeProjection::Vr180 ? kPi : p == DomeProjection::Vr360 ? 2 * kPi : 0.0;
}

DomeSize ComputeDomeSize(DomeProjection p, uint32_t explicitWidth, bool fovValid, const float fovL[4], const float fovR[4],
                         uint32_t wL, uint32_t wR) {
    DomeSize r;
    if (p == DomeProjection::Flat) return r;
    const bool v360 = p == DomeProjection::Vr360;
    const uint32_t align = v360 ? 4u : 2u;
    r.maxEyeW = v360 ? 8192u : 4096u;
    double dens = 0;
    if (fovValid) {
        const float* f[2] = {fovL, fovR};
        const uint32_t w[2] = {wL, wR};
        for (int e = 0; e < 2; ++e) {
            if (!std::isfinite(f[e][0]) || !std::isfinite(f[e][1]) || !(f[e][0] < f[e][1]) || std::fabs(f[e][0]) >= 1.553f ||
                std::fabs(f[e][1]) >= 1.553f || !w[e])
                continue;
            dens = std::max(dens, w[e] / (std::tan((double)f[e][1]) - std::tan((double)f[e][0])));
        }
    }
    r.density = dens;
    if (!(dens > 0)) {   // no usable fov at record start: assume a +-45 degree eye image
        r.fallback = true;
        dens = std::max(wL, wR) / 2.0;
    }
    double want = explicitWidth ? (double)explicitWidth : dens * DomeLonSpan(p);
    r.fromExplicit = explicitWidth != 0;
    uint32_t w = align * (uint32_t)std::lround(want / align);
    if (w < 64) w = 64;
    r.wanted = w;
    if (w > r.maxEyeW) { w = r.maxEyeW; r.clamped = true; }
    r.eyeW = w;
    r.eyeH = v360 ? w / 2 : w;
    r.frameW = v360 ? w : 2 * w;
    r.frameH = v360 ? 2 * r.eyeH : w;
    r.valid = true;
    return r;
}

void DomeRay(double lon, double lat, double r[3]) {
    r[0] = std::cos(lat) * std::sin(lon);
    r[1] = std::sin(lat);
    r[2] = -std::cos(lat) * std::cos(lon);
}

DomeFrame ComputeDomeFrame(const SmoothQuat& qL, const SmoothQuat& qR) {
    DomeFrame f;
    const SmoothQuat H = QuatSlerp(QuatNormalize(qL), QuatNormalize(qR), 0.5);
    double m[9];
    QuatToMatrix(H, m);
    const double fx = -m[2], fz = -m[8];   // H * (0,0,-1), horizontal part
    const double len = std::sqrt(fx * fx + fz * fz);
    double yaw = 0;
    if (len < 0.05) f.degenerate = true;
    else yaw = std::atan2(-fx / len, -fz / len);   // Ry(yaw) * (0,0,-1) = (-sin yaw, 0, -cos yaw)
    f.yawDeg = yaw * 180.0 / kPi;
    f.D.x = 0; f.D.y = std::sin(yaw / 2); f.D.z = 0; f.D.w = std::cos(yaw / 2);
    return f;
}

void DomeEyeMatrix(const SmoothQuat& D, const SmoothQuat& qEye, double m[9]) {
    QuatToMatrix(QuatMul(QuatConj(QuatNormalize(qEye)), D), m);
}

bool DomeDirToUv(DomeProjection p, const SmoothQuat& D, const double d[3], double& u, double& v) {
    double m[9];
    QuatToMatrix(QuatConj(D), m);   // dome-from-space
    const double x = m[0] * d[0] + m[1] * d[1] + m[2] * d[2];
    const double y = m[3] * d[0] + m[4] * d[1] + m[5] * d[2];
    const double z = m[6] * d[0] + m[7] * d[1] + m[8] * d[2];
    const double n = std::sqrt(x * x + y * y + z * z);
    if (!(n > 0)) return false;
    const double lat = std::asin(std::max(-1.0, std::min(1.0, y / n)));
    const double lon = std::atan2(x, -z);
    const double span = DomeLonSpan(p);
    if (span <= 0 || std::fabs(lon) > span / 2) return false;
    u = lon / span + 0.5;
    v = 0.5 - lat / kPi;
    return true;
}
