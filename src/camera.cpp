#include "camera.h"
#include "matrix.h"

#include <cstring>

namespace
{
float s_vp[16];
bool s_have = false;
}

namespace Camera
{
void NewFrame()
{
    s_have = false;
}

void Set(const float* vp)
{
    memcpy(s_vp, vp, sizeof(s_vp));
    s_have = true;
}

const float* Get()
{
    return s_have ? s_vp : nullptr;
}

bool UnitViewProj(IDirect3DDevice9* dev, const ShaderInfo& vs, const float* vp, float* out)
{
    if (!vs.skinWorldViewProj)
    {
        memcpy(out, vp, 16 * sizeof(float));
        return true;
    }
    // Register = camera x placement, so placement = camera^-1 x register, and the new value vp x placement.
    float reg[16];
    double inv[16];
    if (!s_have || FAILED(dev->GetVertexShaderConstantF(static_cast<UINT>(vs.viewProj), reg, 4)) ||
        !mat::Invert(s_vp, inv))
        return false;
    double placement[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += inv[i * 4 + k] * reg[k * 4 + j];
            placement[i * 4 + j] = sum;
        }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += vp[i * 4 + k] * placement[k * 4 + j];
            out[i * 4 + j] = static_cast<float>(sum);
        }
    return true;
}

bool UnitWorldToClip(IDirect3DDevice9* dev, const ShaderInfo& vs, float* out)
{
    if (!vs.skinWorldViewProj)
        return SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(vs.viewProj), out, 4));
    if (!s_have)
        return false;
    memcpy(out, s_vp, sizeof(s_vp));
    return true;
}
}
