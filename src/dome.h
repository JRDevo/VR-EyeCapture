// dome -- CPU side of the VR180 / VR360 dome output ([dome] projection = vr180 | vr360).
//
// The dome is a world-locked equirectangular canvas per eye. Each frame, each eye image is painted into it where the
// eye looked; with fill = last_seen what was painted earlier stays (and fades after fill_fade_s), with fill = black
// everything outside the live window is black. It replaces the flat re-projection (symmetric_fov / head smoothing).
//
// Conventions (OpenXR, see head_smoothing.h): right-handed, -Z forward, +X right, +Y up; a pose orientation q maps
// eye-local vectors into the projection layer's space (v_space = q * v_eye).
//
// Equirect pixel (u, v) in [0,1] of one eye (v down, pixel centres at (i + 0.5) / size):
//     lon = (u - 0.5) * lonSpan        (lonSpan = pi for vr180, 2 pi for vr360; lon > 0 = to the right)
//     lat = (0.5 - v) * pi             (lat > 0 = up)
//     dome ray r = (cos lat sin lon, sin lat, -cos lat cos lon)
// The dome frame D (orientation in the projection layer's space, LOCAL / STAGE = gravity-aligned) is the head's yaw at
// the first recorded frame with a usable pose: H = slerp(qLeft, qRight, 0.5), forward = H * (0,0,-1) projected on the
// horizontal plane (y = 0); pitch = roll = 0 (horizon level). Looking almost straight up / down (horizontal length
// < 0.05): forward = -Z. D is fixed for the whole recording.
// Per eye e (actual orientation q_e): a = M_e r with M_e = q_e^-1 * D maps a dome ray into the eye camera. a.z >= 0 or
// t = a.xy / -a.z outside the eye's fov tans = not covered; else the eye image is sampled at t. Eye positions are not
// touched: each eye's dome is centred on that eye (the stereo baseline, like a VR180 camera).
// Shared view (dome_shared_view = 1, default): each eye paints only the part of its fov the OTHER eye also sees (the
// binocular overlap). A headset eye sees more on its outer side; painted alone, that strip has live video for one eye
// and black / old video for the other -- the eyes cannot fuse it (eye strain, "crossed eyes"; GitHub issue #2).
// The coverage test and the feather use these clip tans; the image is still sampled with the eye's own fov.
// Layout: vr180 = per-eye square (lon +-90, lat +-90), left | right (SBS). vr360 = per-eye 2:1 (lon +-180, lat +-90),
// left eye on top, right eye below (top-bottom, what YouTube 360 3D expects).
#pragma once
#include <cstdint>
#include "head_smoothing.h"

enum class DomeProjection { Flat = 0, Vr180, Vr360 };
enum class DomeFill { Black, LastSeen };

struct DomeSettings {
    DomeProjection projection = DomeProjection::Flat;   // projection = flat | vr180 | vr360
    uint32_t       width = 0;                            // dome_width: per-eye width in px, 0 = from the source density
    DomeFill       fill = DomeFill::LastSeen;            // fill = black | last_seen
    float          fadeS = 8.0f;                         // fill_fade_s: 0 = never fade
    float          featherDeg = 1.0f;                    // window_feather_deg: 0 = hard edge
    bool           sharedView = true;                    // dome_shared_view: paint only where both eyes see
    bool Enabled() const { return projection != DomeProjection::Flat; }
};

const char* DomeProjectionName(DomeProjection p);   // "flat" / "vr180" / "vr360"
const char* DomeFileTag(DomeProjection p);          // "SBS" / "VR180" / "VR360" (file name suffix)
double      DomeLonSpan(DomeProjection p);          // pi (vr180) / 2 pi (vr360) radians; 0 for flat

// Output size.
//   density = max over both eyes of eyeWidth / (tanR - tanL)      (px per unit tan = px per radian at the centre)
//   derived = density * lonSpan; explicit dome_width wins when set
//   per-eye width rounded to even (vr360: to a multiple of 4, so each 2:1 half has an even height), clamped so the
//   frame stays <= 8192 in each dimension: vr180 (SBS 2W x W) W <= 4096, vr360 (top-bottom W x W) W <= 8192.
struct DomeSize {
    bool     valid = false;
    double   density = 0;        // px / rad (0 = no usable fov: fallback used)
    uint32_t wanted = 0;         // per-eye width before clamping (derived or explicit, rounded)
    uint32_t eyeW = 0, eyeH = 0; // per eye
    uint32_t frameW = 0, frameH = 0;
    uint32_t maxEyeW = 0;        // clamp limit
    bool     clamped = false, fromExplicit = false, fallback = false;
};
// fov = angleLeft, angleRight, angleUp, angleDown (radians); fovValid = false -> density fallback eyeWidth / 2.
DomeSize ComputeDomeSize(DomeProjection p, uint32_t explicitWidth, bool fovValid, const float fovL[4], const float fovR[4],
                         uint32_t wL, uint32_t wR);

// Dome ray for lon / lat (radians), see above.
void DomeRay(double lon, double lat, double r[3]);

// Dome frame from the two eye orientations of the first usable frame.
struct DomeFrame {
    SmoothQuat D;              // yaw-only orientation in the projection layer's space
    double     yawDeg = 0;     // rotation about +Y (OpenXR: positive = turned LEFT)
    bool       degenerate = false;   // forward was (almost) vertical: -Z used
};
DomeFrame ComputeDomeFrame(const SmoothQuat& qL, const SmoothQuat& qR);

// Row-major M = q_e^-1 * D: eye-camera ray = M * dome ray.
void DomeEyeMatrix(const SmoothQuat& D, const SmoothQuat& qEye, double m[9]);

// Shared view: the clip window of eye "self" = its own fov intersected with the other eye's fov, as tans in self's
// camera {tanL, tanR, tanU, tanD}. The other eye's edge rays (its centre row / centre column) are rotated into self's
// camera (q_self^-1 * q_other, so canted displays work); positions are ignored (overlap at infinity). Returns false and
// out = self's own tans when the overlap is empty or a ray points backwards.
bool DomeSharedTans(const SmoothQuat& qSelf, const float fovSelf[4], const SmoothQuat& qOther, const float fovOther[4],
                    float out[4]);

// Equirect (u, v) in [0,1] of a direction given in the projection layer's space; false = outside the dome (vr180:
// behind). Used by the tests.
bool DomeDirToUv(DomeProjection p, const SmoothQuat& D, const double dirSpace[3], double& u, double& v);
