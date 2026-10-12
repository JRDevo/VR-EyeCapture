// head_smoothing -- CPU side of head-motion smoothing: a low-pass filter on the head orientation (with adaptive
// follow for deliberate turns) and the per-eye re-projection parameters the recorder's GPU pass uses.
//
// Conventions (OpenXR): right-handed, -Z forward, +X right, +Y up. A view pose orientation q maps eye-local vectors
// into the projection layer's space: v_space = q * v_eye. Fov angles are radians, angleLeft / angleDown negative.
// An eye image pixel at (u, v) in [0,1] (v down) looks along the eye-local ray
//     ( lerp(tan(angleLeft), tan(angleRight), u),  lerp(tan(angleUp), tan(angleDown), v),  -1 ).
//
// Per frame (Update):
//   H  = head orientation = slerp(qLeft, qRight, 0.5)          (the two views' actual rendering orientations)
//   S  = filtered head:  S <- slerp(S, H, a),  a = 1 - exp(-dt / tau)   (tau from head_smoothing)
//        follow: a rises smoothly to 1 while the error angle(S, H) goes from follow_deg to 2 x follow_deg, and once
//        the error has passed follow_deg a catch-up mode (time constant <= 60 ms) stays on until the error is back
//        under follow_deg / 4, so a deliberate turn ends without a slow tail.
//        Reset to H at the first frame and after a gap > 0.5 s.
//   O  = output head orientation = S, without roll if roll_lock.
//        edge_guard: if the output frame would need pixels the game did not render (black edges), O is moved
//        towards H just enough (bisection on slerp(O, H, b)) that both eyes are covered. Only O is changed; the
//        filter state S keeps filtering.
//   per eye e (actual orientation q_e): smoothed eye = O * H^-1 * q_e (keeps the eye's own offset from the head,
//        e.g. canted displays); GPU matrix C_e = q_e^-1 * O * H^-1 * q_e maps a ray of the smoothed (output) camera
//        into the actual eye camera. Output fov = source fov with each tan scaled by output_fov_scale.
//   headCorr = O * H^-1 (SmoothFrame; the same for the symmetric output below): head-locked overlay correction.
// Translation is ignored on purpose (needs depth; negligible at scene distances).
//
// Symmetric output (symmetric_fov = 1, default): headsets render each eye with an asymmetric fov (outer angle larger
// than the inner one). A flat 3D SBS player shows both halves on the same rectangle, so with the raw images a far
// object lands at different x in the two halves (parallax even at infinity -> the eyes do not fuse). With a
// symmetric output both eyes get the SAME symmetric frustum (tanH = smallest |tan| of the four horizontal angles,
// tanV likewise = the common coverage, x output_fov_scale only while smoothing) and the SAME orientation (the head:
// O = H without smoothing, the smoothed O with it), i.e. parallel cameras: C_e = q_e^-1 * O. Eye positions are kept
// (that is the stereo baseline). The stage then runs even with head_smoothing = 0.
#pragma once
#include <cstdint>

struct HeadSmoothingSettings {
    float smoothing = 0.0f;      // head_smoothing 0..1, 0 = off (stage bypassed)
    float followDeg = 12.0f;     // head_follow_deg
    bool  rollLock = false;      // roll_lock
    float fovScale = 0.9f;       // output_fov_scale 0.5..1
    bool  edgeGuard = true;      // edge_guard
    bool  symmetricFov = true;   // symmetric_fov
    bool Enabled() const { return smoothing > 0.0f; }   // the orientation filter
};

// Common symmetric fov of two eyes and the per-eye output size at the source pixel density.
//   tanH = min over both eyes of min(|tan angleLeft|, |tan angleRight|), tanV likewise (up / down)
//   density = max over both eyes of eyeWidth / (tanR - tanL)  (and height / (tanU - tanD))
//   eyeW = round_even(2 tanH densityH), eyeH = round_even(2 tanV densityV)
struct SymmetricFov {
    bool     valid = false;
    double   tanH = 0, tanV = 0;
    uint32_t eyeW = 0, eyeH = 0;
};
// fov = angleLeft, angleRight, angleUp, angleDown (radians); w / h = the eye's source image size.
SymmetricFov ComputeSymmetricFov(const float fovL[4], const float fovR[4], uint32_t wL, uint32_t hL, uint32_t wR, uint32_t hR);

// head_smoothing -> filter time constant in seconds: 0.05 * 30^((s - 0.1) / 0.9)
// (0.1 -> 0.05 s, 0.3 -> 0.11 s, 0.5 -> 0.23 s, 0.6 -> 0.33 s, 0.7 -> 0.48 s, 0.8 -> 0.70 s, 1.0 -> 1.5 s).
double HeadSmoothingTimeConstant(float smoothing);

struct SmoothQuat { double x = 0, y = 0, z = 0, w = 1; };

// Quaternion helpers (Hamilton product, unit quaternions; also used by dome.cpp).
SmoothQuat QuatMul(const SmoothQuat& a, const SmoothQuat& b);
SmoothQuat QuatConj(const SmoothQuat& a);
SmoothQuat QuatNormalize(const SmoothQuat& a);
SmoothQuat QuatSlerp(const SmoothQuat& a, const SmoothQuat& b, double t);
void       QuatToMatrix(const SmoothQuat& q, double m[9]);   // row-major, v' = M v

struct SmoothEyeIn {
    bool valid = false;              // pose and fov usable
    SmoothQuat q;                    // actual eye orientation
    double tanL = -1, tanR = 1, tanU = 1, tanD = -1;   // tan of the fov angles
};

struct SmoothEyeOut {
    float m[9];          // row-major C: actual-eye ray = C * output ray
    float outTan[4];     // tanL, tanR, tanU, tanD of the output camera (scaled)
    float srcTan[4];     // tanL, tanR, tanU, tanD of the source image
    bool  covered;       // the whole output frustum lies inside the source frustum
};

struct SmoothFrame {
    SmoothEyeOut eye[2];
    double correctionDeg = 0;   // angle between output head and actual head
    bool   reset = false;       // filter (re)started on this frame
    bool   guarded = false;     // edge guard moved the output this frame
    bool   uncovered = false;   // some output pixels have no source (black)
    bool   catchUp = false;     // follow / catch-up active
    bool   noPose = false;      // no usable pose on this frame -> pass-through mapping
    // O * H^-1 (projection space): actual head -> output head (identity without a pose). A point P moved to
    // p_e + headCorr (P - p_e) (p_e = an eye's position) lands, after the re-projection, on the output pixel where P is
    // without smoothing -- record_overlays uses it to keep head-locked (VIEW-space) layers fixed to the output camera.
    SmoothQuat headCorr;
};

class HeadSmoother {
public:
    explicit HeadSmoother(const HeadSmoothingSettings& s = HeadSmoothingSettings()) { Configure(s); }
    void Configure(const HeadSmoothingSettings& s);
    // Symmetric output frustum (tanH, tanV > 0) for both eyes, or 0 = output fov = each eye's own (scaled) fov.
    void SetSymmetricOutput(double tanH, double tanV) { symH_ = tanH; symV_ = tanV; }
    void Reset() { have_ = false; catch_ = 0; }
    // displayTimeNs: the frame's XrFrameEndInfo::displayTime (any monotonic ns clock).
    void Update(int64_t displayTimeNs, const SmoothEyeIn in[2], SmoothFrame& out);

private:
    HeadSmoothingSettings s_;
    double tau_ = 0.33, tauFast_ = 0.06;
    bool have_ = false;
    SmoothQuat S_;
    int64_t lastNs_ = 0;
    double catch_ = 0;    // 0..1 catch-up level
    double symH_ = 0, symV_ = 0;
};
