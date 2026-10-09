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
// Translation is ignored on purpose (needs depth; negligible at scene distances).
#pragma once
#include <cstdint>

struct HeadSmoothingSettings {
    float smoothing = 0.0f;      // head_smoothing 0..1, 0 = off (stage bypassed)
    float followDeg = 12.0f;     // head_follow_deg
    bool  rollLock = false;      // roll_lock
    float fovScale = 0.9f;       // output_fov_scale 0.5..1
    bool  edgeGuard = true;      // edge_guard
    bool Enabled() const { return smoothing > 0.0f; }
};

// head_smoothing -> filter time constant in seconds: 0.05 * 30^((s - 0.1) / 0.9)
// (0.1 -> 0.05 s, 0.3 -> 0.11 s, 0.5 -> 0.23 s, 0.6 -> 0.33 s, 0.7 -> 0.48 s, 0.8 -> 0.70 s, 1.0 -> 1.5 s).
double HeadSmoothingTimeConstant(float smoothing);

struct SmoothQuat { double x = 0, y = 0, z = 0, w = 1; };

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
};

class HeadSmoother {
public:
    explicit HeadSmoother(const HeadSmoothingSettings& s = HeadSmoothingSettings()) { Configure(s); }
    void Configure(const HeadSmoothingSettings& s);
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
};
