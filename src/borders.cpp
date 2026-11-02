#include "borders.h"
#include "border_ps.h"
#include "border_vs.h"
#include "matrix.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
// How far above the ground a lifted vertex sits: the terrain's triangles and the bilinear heights differ a little
// between grid points, as for the draped waves.
const float kLift = 1.5f;
// How far towards the camera the border is moved (world units; a plot is 180 across): the most that the terrain may
// rise in front of it between its vertices (11.25 apart) and still not cover it. Dune Wars' cliffs, 65 samples a plot.
const float kPull = 24.0f;
const int kRegister = 14, kRegisters = 6;  // c14-c17 the view-projection, c18 the grid, lift and pull, c19 the camera

template <class T>
std::vector<DWORD> Tokens(const T& code)
{
    std::vector<DWORD> t(sizeof(code) / 4);
    memcpy(t.data(), code, sizeof(code));
    return t;
}
}

bool Borders::IsBorder(const ShaderInfo& vs, const ShaderInfo* ps)
{
    // CultureBOrderShader.fx's registers, as our copy has them: P0 with texture matrices and colours 1 and 2, P1 with
    // 3 and 4.
    const bool vsOk = vs.Reg("mtxWorldViewProj", 2) == 0 && vs.Reg("mtxWorld", 2) == 4 && vs.Reg("mtxFOW", 2) == 8 &&
                      ((vs.Reg("mtxBorderTextureMat1", 2) == 10 && vs.Reg("mtxBorderTextureMat2", 2) == 12) ||
                       (vs.Reg("mtxBorderTextureMat3", 2) == 10 && vs.Reg("mtxBorderTextureMat4", 2) == 12));
    return vsOk && ps && ps->Reg("CultureBorderBase", 3) == 0 && ps->Reg("TerrainFOWar", 3) == 2 &&
           ((ps->Reg("f4BorderColor1", 2) == 0 && ps->Reg("f4BorderColor2", 2) == 1) ||
            (ps->Reg("f4BorderColor3", 2) == 0 && ps->Reg("f4BorderColor4", 2) == 1));
}

bool Borders::EnsureShaders()
{
    if (m_tried)
        return m_vs != nullptr;
    m_tried = true;
    D3DCAPS9 caps = {};
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    D3DDISPLAYMODE mode = {};
    bool vertexTextures = false;
    if (SUCCEEDED(m_dev->GetDeviceCaps(&caps)) && caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) &&
        caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) && SUCCEEDED(m_dev->GetDirect3D(&d3d)) &&
        SUCCEEDED(m_dev->GetCreationParameters(&cp)) && SUCCEEDED(m_dev->GetDisplayMode(0, &mode)))
        vertexTextures = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                          D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_R32F));
    if (d3d)
        d3d->Release();
    if (!vertexTextures)
    {
        Log::Write("borders: the card can't read heights in a vertex shader (Shader Model 3 vertex textures); culture "
                   "borders drawn as the game draws them");
        return false;
    }
    ULONG before = DeviceRefs(m_dev);
    if (FAILED(m_dev->CreateVertexShader(Tokens(g_borderVS).data(), &m_vs)) ||
        FAILED(m_dev->CreatePixelShader(Tokens(g_borderPS).data(), &m_ps)))
    {
        if (m_vs)
            m_vs->Release();
        m_vs = nullptr;
        m_ps = nullptr;
        Log::Write("borders: creating the border shaders failed; culture borders drawn as the game draws them");
        return false;
    }
    m_internalRefs += DeviceRefs(m_dev) - before;
    return true;
}

bool Borders::Draw(const std::function<HRESULT()>& draw, HRESULT& hr)
{
    if (!EnsureShaders())
        return false;
    // The camera's view-projection from the game's constants (world -> clip = world-view-projection x the inverse of
    // the placement), as the water and the waves do; the camera itself where the view-projection sends a point to
    // w = 0 at the screen's centre.
    float wvp[16], world[16], worldInv[16], vp[16], vpInv[16], consts[kRegisters * 4] = {};
    if (FAILED(m_dev->GetVertexShaderConstantF(0, wvp, 4)) || FAILED(m_dev->GetVertexShaderConstantF(4, world, 4)) ||
        !mat::Invert(world, worldInv))
        return false;
    mat::Mul(wvp, worldInv, vp);
    if (!mat::Invert(vp, vpInv) || std::fabs(vpInv[14]) < 1e-12f)
        return false;  // no inverse, or no perspective (no camera point): as the game draws it
    memcpy(consts, vp, sizeof(vp));
    const bool heights = heightTexture && heightSpacing > 0 && heightCells > 0;
    consts[16] = heights ? 1.0f / heightSpacing : 0;
    consts[17] = heights ? 1.0f / heightCells : 0;
    consts[18] = heights ? kLift : -1e6f;  // no heights yet: nothing is lifted
    consts[19] = kPull;
    consts[20] = vpInv[2] / vpInv[14];
    consts[21] = vpInv[6] / vpInv[14];
    consts[22] = vpInv[10] / vpInv[14];
    consts[23] = 1;

    float old[kRegisters * 4];
    m_dev->GetVertexShaderConstantF(kRegister, old, kRegisters);
    m_dev->SetVertexShaderConstantF(kRegister, consts, kRegisters);
    IDirect3DBaseTexture9* oldHeights = nullptr;
    const D3DSAMPLERSTATETYPE states[5] = {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                           D3DSAMP_MIPFILTER};
    const DWORD values[5] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE};
    DWORD oldStates[5] = {};
    m_dev->GetTexture(D3DVERTEXTEXTURESAMPLER0, &oldHeights);
    m_dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, heights ? heightTexture : nullptr);
    for (int k = 0; k < 5; ++k)
    {
        m_dev->GetSamplerState(D3DVERTEXTEXTURESAMPLER0, states[k], &oldStates[k]);
        m_dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, states[k], values[k]);
    }
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    m_dev->GetVertexShader(&oldVs);
    m_dev->GetPixelShader(&oldPs);
    m_dev->SetVertexShader(m_vs);
    m_dev->SetPixelShader(m_ps);
    hr = draw();
    m_dev->SetVertexShader(oldVs);
    m_dev->SetPixelShader(oldPs);
    if (oldVs)
        oldVs->Release();
    if (oldPs)
        oldPs->Release();
    m_dev->SetVertexShaderConstantF(kRegister, old, kRegisters);
    m_dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, oldHeights);
    if (oldHeights)
        oldHeights->Release();
    for (int k = 0; k < 5; ++k)
        m_dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, states[k], oldStates[k]);
    if (!m_logged || (heights && !m_loggedHeights))
    {
        m_logged = true;
        m_loggedHeights = heights;
        Log::Write("borders: culture borders drawn with our border shader, %.0f units nearer the camera; %s", kPull,
                   heights ? "lifted onto the terrain" : "not lifted: no terrain heights read yet");
    }
    return true;
}

void Borders::ReleaseAll()
{
    // No reference counting afterwards: this can be the device's last object.
    IUnknown* objects[] = {m_vs, m_ps};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_vs = nullptr;
    m_ps = nullptr;
    m_tried = false;
    m_internalRefs = 0;
}
