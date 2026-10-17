// reproject -- see reproject.h.
#include "reproject.h"
#include <cmath>

bool ReprojPoseUsable(const EyeView& v) {
    bool ok = v.hasPose;
    for (int i = 0; ok && i < 4; ++i) ok = std::isfinite(v.fov[i]) && std::fabs(v.fov[i]) < 1.553f && std::isfinite(v.orientation[i]);
    return ok && v.fov[0] < v.fov[1] && v.fov[3] < v.fov[2];
}

SmoothEyeIn ReprojSmoothIn(bool valid, const float q[4], const float fov[4]) {
    SmoothEyeIn in;
    in.valid = valid;
    in.q.x = q[0]; in.q.y = q[1]; in.q.z = q[2]; in.q.w = q[3];
    in.tanL = std::tan((double)fov[0]); in.tanR = std::tan((double)fov[1]);
    in.tanU = std::tan((double)fov[2]); in.tanD = std::tan((double)fov[3]);
    return in;
}

void ReprojEyeMapping(const SmoothEyeOut& eo, RpEye& re) {
    for (int c = 0; c < 3; ++c) { re.r0[c] = eo.m[c]; re.r1[c] = eo.m[3 + c]; re.r2[c] = eo.m[6 + c]; }
    for (int c = 0; c < 4; ++c) { re.outTan[c] = eo.outTan[c]; re.srcTan[c] = eo.srcTan[c]; }
}

void ReprojIdentityMapping(RpEye& re) {
    const float r[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    const float tan[4] = {-1.0f, 1.0f, 1.0f, -1.0f};   // left, right, up, down: any frustum works when both are the same
    for (int c = 0; c < 4; ++c) {
        re.r0[c] = r[0][c]; re.r1[c] = r[1][c]; re.r2[c] = r[2][c];
        re.outTan[c] = tan[c]; re.srcTan[c] = tan[c];
    }
}

void ReprojDraw(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11Buffer* cb,
                ID3D11SamplerState* smp, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
                ID3D11RenderTargetView* const* rtvs, UINT nRtv, UINT w, UINT h) {
    const D3D11_VIEWPORT vpt{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, nSrv, srvs);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vpt);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetRenderTargets(nRtv, rtvs, nullptr);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[8] = {};
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->PSSetShaderResources(0, nSrv < 8 ? nSrv : 8, nullSrv);
    ctx->VSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
}
