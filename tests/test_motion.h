// test_motion -- the head motion the mock runtime reports in --jitter mode (EYECAP_MOCK_MOTION=jitter), shared by
// tests/mock_runtime.cpp and tests/layer_e2e.cpp. tests/run_jitter.ps1 has the same formula (C#) for its checks.
//
// t = seconds since the first predictedDisplayTime of the session.
//   jitter (whole run): yaw   2 deg * sin(2 pi 3.3 t)
//                       pitch 2 deg * sin(2 pi 4.1 t + 1.0)
//                       roll  2 deg * sin(2 pi 4.7 t + 2.0)
//   deliberate turn:    yaw  += 30 deg, cosine ease from t = 4.0 s to t = 5.0 s (turning LEFT: +yaw about +Y)
//   orientation q = Ry(yaw) * Rx(pitch) * Rz(roll)      (OpenXR: right-handed, -Z forward, +Y up)
// Per-eye fov (asymmetric like a real headset, radians): left {-0.87, 0.75, 0.80, -0.85}, right mirrored.
// Eyes share the head orientation; positions are +-32 mm along the head's +X.
#pragma once
#include <cmath>

namespace test_motion {

const double kPi = 3.14159265358979323846;
const double kDeg = kPi / 180.0;
const double kTurnStart = 4.0, kTurnEnd = 5.0, kTurnDeg = 30.0, kJitterDeg = 2.0;
const float kFovLeft[4] = {-0.87f, 0.75f, 0.80f, -0.85f};
const float kFovRight[4] = {-0.75f, 0.87f, 0.80f, -0.85f};

struct Quat { double x, y, z, w; };

inline Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Axis(double ax, double ay, double az, double ang) {
    const double s = std::sin(ang / 2);
    return {ax * s, ay * s, az * s, std::cos(ang / 2)};
}

inline void Angles(double t, double& yaw, double& pitch, double& roll) {
    yaw = kJitterDeg * std::sin(2 * kPi * 3.3 * t);
    pitch = kJitterDeg * std::sin(2 * kPi * 4.1 * t + 1.0);
    roll = kJitterDeg * std::sin(2 * kPi * 4.7 * t + 2.0);
    if (t >= kTurnEnd) yaw += kTurnDeg;
    else if (t > kTurnStart) yaw += kTurnDeg * (0.5 - 0.5 * std::cos(kPi * (t - kTurnStart) / (kTurnEnd - kTurnStart)));
}

inline Quat Head(double t) {
    double yaw, pitch, roll;
    Angles(t, yaw, pitch, roll);
    return Mul(Axis(0, 1, 0, yaw * kDeg), Mul(Axis(1, 0, 0, pitch * kDeg), Axis(0, 0, 1, roll * kDeg)));
}

// Row-major rotation matrix (v_space = M * v_eye).
inline void Matrix(const Quat& q, float m[9]) {
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = (float)(1 - 2 * (yy + zz)); m[1] = (float)(2 * (xy - wz));     m[2] = (float)(2 * (xz + wy));
    m[3] = (float)(2 * (xy + wz));     m[4] = (float)(1 - 2 * (xx + zz)); m[5] = (float)(2 * (yz - wx));
    m[6] = (float)(2 * (xz - wy));     m[7] = (float)(2 * (yz + wx));     m[8] = (float)(1 - 2 * (xx + yy));
}

} // namespace test_motion
