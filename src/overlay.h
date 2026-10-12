// overlay -- record_overlays: the game's / other mods' OpenXR composition layers (menus, the DLAA injector's VR panel and
// fps box, 360 backdrops) composited into OUR copy of the eye images, the way the runtime would show them.
//
// Data path (see docs/DEVELOPMENT.md "Overlays"):
//   game thread (xrEndFrame, layer.cpp CollectOverlays): quad / cylinder / equirect (v1, v2) / cube layers, back to front:
//     the ones BEFORE the captured projection layer (only when that layer has BLEND_TEXTURE_SOURCE_ALPHA: they show
//     through its transparent pixels) and the ones AFTER it. Each with its swapchain image of this frame (the most
//     recently released one), sub-image rect / slice, eye visibility, blend flags, an XrCompositionLayerColorScaleBiasKHR
//     from its next chain and its pose located in the projection layer's space at displayTime -> OverlayFrame.
//   Recorder::SubmitFrame (game thread): packs the sub-images into this ring slot's overlay ATLAS of the layer's format
//     family (one shared texture per family and ring slot, the family's typed UNORM / float member, created lazily by the
//     worker at the first frame that has a layer of that family) -> IFrameSource::CopyEyes copies them next to the eye
//     copies (same fences); MSAA swapchains are resolved first; a cube layer's 6 faces go in as a 3 x 2 block.
//   encoder worker (OverlayCompositor::Draw, inside the device lock, BEFORE the linear pre-pass / re-projection / dome
//     pass), per output eye: [layers behind the projection: the eye is saved, cleared to black, those layers drawn, then
//     the saved projection is blended over them with its own alpha] then the layers after it. Quad / cylinder: their mesh
//     projected with that eye's submitted pose + fov (hardware clipping, perspective-correct uv, back faces culled).
//     Equirect / cube: one full-eye draw, a ray per pixel from the eye's pose + fov into the layer's frame, then the
//     equirect (sphere of `radius`, 0 / inf = infinitely far) or cube-face lookup. Everything is blended in LINEAR light:
//     the destination region is copied to a scratch texture first and the pixel shader decodes / encodes sRGB itself
//     (the ring is a typed UNORM texture holding sRGB-encoded bytes).
// The game's swapchain images are only ever copied from (or resolved from), exactly like the eye images. The in-headset
// indicator is appended after the capture and is never part of this.
#pragma once
#include <cstdint>
#include <vector>
#include "frame_source.h"

const uint32_t kMaxOverlays = 16;         // overlay layers per frame (more: the rest is skipped, logged)
const UINT     kOverlayAtlasSize = 4096;  // atlas per family and ring slot, square (16 MB per 1024 x 1024 of 8-bit RGBA)
const uint32_t kMaxOverlayCopies = kMaxOverlays * 6;   // sub-image copies per frame (a cube layer needs 6)

enum class OverlayKind : uint8_t { Quad, Cylinder, Equirect, Equirect2, Cube };
const char* OverlayKindName(OverlayKind k);
inline bool OverlayKindIsRay(OverlayKind k) { return k == OverlayKind::Equirect || k == OverlayKind::Equirect2 || k == OverlayKind::Cube; }

// One overlay layer of one frame (layer.cpp -> Recorder::SubmitFrame).
struct OverlayLayer {
    OverlayKind kind = OverlayKind::Quad;
    uint8_t  eyeMask = 3;                 // bit e = drawn into OUTPUT eye e (0 = left half; after swap_eyes)
    bool     alpha = false;               // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT (else opaque)
    bool     unpremultiplied = false;     // XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (else premultiplied colour)
    bool     srgb = false;                // the swapchain format is *_SRGB: its texels are sRGB-decoded before filtering
    bool     behind = false;              // submitted BEFORE the projection layer (seen through its transparent pixels)
    int      family = -1;                 // OverlayFamily() of the swapchain format: which atlas
    // XrCompositionLayerColorScaleBiasKHR (identity when absent): colour = colour * scale + bias (linear, after sampling)
    float    colorScale[4] = {1, 1, 1, 1};
    float    colorBias[4] = {0, 0, 0, 0};
    // the sub-image of this frame (cube: the whole image, faces imageArrayIndex * 6 + 0..5 starting at arrayIndex)
    void*                image = nullptr; // ID3D11Texture2D* / ID3D12Resource*
    const SwapchainInfo* sc = nullptr;
    const void*          scKey = nullptr; // identity of the swapchain (MSAA resolve target cache)
    uint32_t             arrayIndex = 0;
    int32_t              x = 0, y = 0;
    uint32_t             w = 0, h = 0;
    // geometry, in the projection layer's space (OpenXR: right-handed, -Z forward, +Y up)
    float    q[4] = {0, 0, 0, 1};         // x, y, z, w
    float    p[3] = {0, 0, 0};
    float    width = 0, height = 0;       // quad: size in metres (centred on the pose, front face = +Z)
    float    radius = 0, centralAngle = 0, aspect = 1;   // cylinder: axis = +Y of the pose, arc centred on -Z, inside visible
    // equirect (both): radius = sphere radius (0 / inf = infinite), centred on the pose. v1: uv = (azimuth / 2pi + 0.5,
    // 0.5 - elevation / pi) * scale + bias; v2: centralHorizontalAngle around -Z, upper / lower vertical angle.
    float    eqScale[2] = {1, 1}, eqBias[2] = {0, 0};
    float    centralHorizontalAngle = 0, upperVerticalAngle = 0, lowerVerticalAngle = 0;
    // cube: only q is used (infinitely far, centred on the eye)
};
struct OverlayFrame {
    uint32_t     n = 0;
    uint32_t     nBehind = 0;             // layer[0 .. nBehind) are behind the projection layer
    bool         projUnpremultiplied = false;   // the projection layer's UNPREMULTIPLIED_ALPHA flag (nBehind > 0 only)
    OverlayLayer layer[kMaxOverlays];
};

// What the worker draws: the layer + where its pixels are in its family's atlas of this slot.
struct OverlayDraw {
    OverlayLayer l;
    uint32_t     ax = 0, ay = 0;          // top-left of the sub-image in the atlas (size l.w x l.h; cube: 3 x 2 faces)
};

// One output eye as the worker sees it (pose / fov as submitted, its rectangle in the ring slot).
struct OverlayEye {
    bool  valid = false;
    float q[4] = {0, 0, 0, 1}, p[3] = {0, 0, 0};
    float fov[4] = {0, 0, 0, 0};          // angleLeft, angleRight, angleUp, angleDown (radians)
    UINT  x0 = 0, w = 0, h = 0;
};

// Typeless member of a DXGI format's copy family (R8G8B8A8 / B8G8R8A8 / B8G8R8X8 / R10G10B10A2 / R16G16B16A16), or
// DXGI_FORMAT_UNKNOWN (R11G11B10_FLOAT has no typeless member: itself).
DXGI_FORMAT TypelessForDxgi(DXGI_FORMAT f);
// Atlas family index 0 .. kOverlayFamilies-1 of a swapchain format (-1 = not supported), its typed atlas format and name.
int         OverlayFamily(DXGI_FORMAT f);
DXGI_FORMAT OverlayFamilyFormat(int family);
const char* OverlayFamilyName(int family);
UINT        OverlayFamilyBytes(int family);

// CPU part of the mesh draw (also used by the tests): the layer's triangle strip in the eye's clip space.
// out: x, y, z, w, u, v per vertex (u, v in 0..1 of the sub-image, v down). Returns the vertex count (0 = nothing to
// draw: bad parameters, or an equirect / cube layer). The z / w pair clips everything nearer than 1 cm.
struct OverlayVertex { float x, y, z, w, u, v; };
const int kOverlayMaxVertices = 2 * (64 + 1);
int OverlayClipVertices(const OverlayLayer& l, const OverlayEye& e, OverlayVertex* out, int maxVerts);
// Pixel bounding box of those vertices inside the eye's rectangle (whole rectangle if a vertex is behind the near
// plane). false = empty (entirely outside).
bool OverlayPixelBox(const OverlayVertex* v, int n, const OverlayEye& e, UINT& x0, UINT& y0, UINT& x1, UINT& y1);

// Equirect / cube layers: the eye-camera -> layer-frame rotation (rows) and the eye position in the layer frame.
struct OverlayRay { float r[3][3]; float o[3]; };
bool OverlayRayFor(const OverlayLayer& l, const OverlayEye& e, OverlayRay& out);
// CPU mirror of the pixel shader's lookup (tests): the pixel centre (px, py) of the side-by-side frame in eye e ->
// texture coordinate (u, v in 0..1 of the sub-image, or of the cube face `face` 0..5 = +X -X +Y -Y +Z -Z).
// false = the layer does not cover that pixel.
bool OverlayRayLookup(const OverlayLayer& l, const OverlayEye& e, double px, double py, double& u, double& v, int& face);

// Encoder worker side.
class OverlayCompositor {
public:
    // ring: the ring slots on the encoder device (typed ringFmt, render-target capable); ringSrgb = the ring holds
    // sRGB-encoded bytes. Atlases are added per family with AddAtlas.
    bool Init(ID3D11Device* dev, const std::vector<ComPtr<ID3D11Texture2D>>& ring, DXGI_FORMAT ringFmt, bool ringSrgb);
    // atlas: one per slot, typed (UNORM / float) atlasFmt, atlasW x atlasH, for format family `family`.
    bool AddAtlas(int family, const std::vector<ComPtr<ID3D11Texture2D>>& atlas, DXGI_FORMAT atlasFmt);
    // Inside the encoder device lock, after the ring slot's ready fence was waited for. d[0 .. nBehind) are behind the
    // projection layer (projUnpremul = its UNPREMULTIPLIED_ALPHA flag). Returns the number of (layer, eye) draws made.
    // Leaves no state bound that the later passes rely on.
    int Draw(ID3D11DeviceContext* ctx, UINT slot, const OverlayEye eyes[2], const OverlayDraw* d, uint32_t n,
             uint32_t nBehind = 0, bool projUnpremul = false);
    void Release();
    bool Ready() const { return ready_; }
    bool HasFamily(int f) const { return f >= 0 && f < kOverlayFamilies && !atlasSrv_[f].empty(); }
    double ScratchMB() const;

private:
    bool EnsureTex(ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11ShaderResourceView>& v, UINT& tw, UINT& th, UINT w, UINT h, const char* what);
    bool Bind(ID3D11DeviceContext* ctx);
    bool DrawLayer(ID3D11DeviceContext* ctx, UINT slot, const OverlayEye& E, int e, const OverlayDraw& dr, uint32_t k);
    ComPtr<ID3D11Device> dev_;
    std::vector<ComPtr<ID3D11RenderTargetView>> ringRtv_;
    std::vector<ComPtr<ID3D11Texture2D>> ring_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> atlasSrv_[kOverlayFamilies];
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_, psRay_, psOver_;
    ComPtr<ID3D11InputLayout> il_;
    ComPtr<ID3D11Buffer> vb_, cb_;
    ComPtr<ID3D11RasterizerState> rs_;
    ComPtr<ID3D11Texture2D> scratch_, proj_;
    ComPtr<ID3D11ShaderResourceView> scratchSrv_, projSrv_;
    UINT scratchW_ = 0, scratchH_ = 0, projW_ = 0, projH_ = 0, ringW_ = 0, ringH_ = 0;
    DXGI_FORMAT ringFmt_ = DXGI_FORMAT_UNKNOWN;
    bool ringSrgb_ = true, ready_ = false, loggedScratch_ = false, bound_ = false;
    int debugDraws_ = 24;   // log_level = debug: the first draws are logged
};
