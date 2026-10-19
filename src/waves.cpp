#include "waves.h"
#include "matrix.h"
#include "wave_drape_ps.h"
#include "wave_drape_vs.h"
#include "wave_ps.h"
#include "wave_vs.h"

#include <cstring>
#include <vector>

namespace
{
// How far above the ground draped foam sits: the terrain's triangles and the bilinear heights differ a little between
// grid points, so a margin keeps the foam from sinking into the beach.
const float kLift = 1.5f;
const int kDrapeRegister = 13, kDrapeRegisters = 5;  // c13-c16 the view-projection, c17 the grid and lift

template <class T>
std::vector<DWORD> Tokens(const T& code)
{
    std::vector<DWORD> t(sizeof(code) / 4);
    memcpy(t.data(), code, sizeof(code));
    return t;
}
}

bool Waves::IsWave(const ShaderInfo& vs, const ShaderInfo* ps)
{
    // Civ4Wave.fx's registers, as our copy has them.
    return ps && vs.Reg("mtxWorldViewProj", 2) == 0 && vs.Reg("mtxWorld", 2) == 4 && vs.Reg("mtxFOW", 2) == 7 &&
           vs.Reg("mtxBaseTexture", 2) == 9 && vs.Reg("mtxDecalTexture", 2) == 11 && ps->Reg("WaveBase", 3) == 0 &&
           ps->Reg("DecalBase", 3) == 1 && ps->Reg("Fog", 3) == 2;
}

bool Waves::EnsureShaders()
{
    if (m_vs || m_failed)
        return m_vs != nullptr;
    ULONG before = DeviceRefs(m_dev);
    if (FAILED(m_dev->CreateVertexShader(Tokens(g_waveVS).data(), &m_vs)) ||
        FAILED(m_dev->CreatePixelShader(Tokens(g_wavePS).data(), &m_ps)))
    {
        if (m_vs)
            m_vs->Release();
        m_vs = nullptr;
        m_ps = nullptr;
        m_failed = true;
        Log::Write("waves: creating the wave shaders failed; waves drawn as the game draws them");
        return false;
    }
    m_internalRefs += DeviceRefs(m_dev) - before;
    return true;
}

bool Waves::EnsureDrape()
{
    if (m_drapeTried)
        return m_drapeVs != nullptr;
    m_drapeTried = true;
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
        Log::Write("waves: the card can't read heights in a vertex shader (Shader Model 3 vertex textures); waves stay flat");
        return false;
    }
    ULONG before = DeviceRefs(m_dev);
    if (FAILED(m_dev->CreateVertexShader(Tokens(g_waveDrapeVS).data(), &m_drapeVs)) ||
        FAILED(m_dev->CreatePixelShader(Tokens(g_waveDrapePS).data(), &m_drapePs)))
    {
        if (m_drapeVs)
            m_drapeVs->Release();
        m_drapeVs = nullptr;
        m_drapePs = nullptr;
        Log::Write("waves: creating the draped wave shaders failed; waves stay flat");
        return false;
    }
    m_internalRefs += DeviceRefs(m_dev) - before;
    return true;
}

bool Waves::Draw(const std::function<HRESULT()>& draw, HRESULT& hr)
{
    if (!EnsureShaders())
        return false;
    // Draped: the camera's view-projection from the game's constants (world -> clip = world-view-projection x the
    // inverse of the placement), as the water does.
    float wvp[16], world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, worldInv[16], drape[20];
    bool draped = heightTexture && heightSpacing > 0 && heightCells > 0 && EnsureDrape() &&
                  SUCCEEDED(m_dev->GetVertexShaderConstantF(0, wvp, 4)) &&
                  SUCCEEDED(m_dev->GetVertexShaderConstantF(4, world, 3));
    if (draped)
    {
        world[12] = world[13] = world[14] = 0;
        world[15] = 1;
        draped = mat::Invert(world, worldInv);
    }
    float oldDrape[kDrapeRegisters * 4];
    IDirect3DBaseTexture9* oldHeights = nullptr;
    const D3DSAMPLERSTATETYPE states[5] = {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                           D3DSAMP_MIPFILTER};
    const DWORD values[5] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE};
    DWORD oldStates[5] = {};
    if (draped)
    {
        mat::Mul(wvp, worldInv, drape);
        drape[16] = 1.0f / heightSpacing;
        drape[17] = 1.0f / heightCells;
        drape[18] = kLift;
        drape[19] = 0;
        m_dev->GetVertexShaderConstantF(kDrapeRegister, oldDrape, kDrapeRegisters);
        m_dev->SetVertexShaderConstantF(kDrapeRegister, drape, kDrapeRegisters);
        m_dev->GetTexture(D3DVERTEXTEXTURESAMPLER0, &oldHeights);
        m_dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, heightTexture);
        for (int k = 0; k < 5; ++k)
        {
            m_dev->GetSamplerState(D3DVERTEXTEXTURESAMPLER0, states[k], &oldStates[k]);
            m_dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, states[k], values[k]);
        }
    }
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    m_dev->GetVertexShader(&oldVs);
    m_dev->GetPixelShader(&oldPs);
    float oldLight[4];
    m_dev->GetPixelShaderConstantF(0, oldLight, 1);
    // [water] FoamLight of the change of light: 0 = the white BtS draws, 1 = all of it, as the roads take it.
    const float k = g_config.foamLight;
    const float lightNow[4] = {light ? 1 + (light[0] - 1) * k : 1.0f, light ? 1 + (light[1] - 1) * k : 1.0f,
                               light ? 1 + (light[2] - 1) * k : 1.0f, 1};
    m_dev->SetPixelShaderConstantF(0, lightNow, 1);
    m_dev->SetVertexShader(draped ? m_drapeVs : m_vs);
    m_dev->SetPixelShader(draped ? m_drapePs : m_ps);
    hr = draw();
    m_dev->SetVertexShader(oldVs);
    m_dev->SetPixelShader(oldPs);
    m_dev->SetPixelShaderConstantF(0, oldLight, 1);
    if (oldVs)
        oldVs->Release();
    if (oldPs)
        oldPs->Release();
    if (draped)
    {
        m_dev->SetVertexShaderConstantF(kDrapeRegister, oldDrape, kDrapeRegisters);
        m_dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, oldHeights);
        if (oldHeights)
            oldHeights->Release();
        for (int k = 0; k < 5; ++k)
            m_dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, states[k], oldStates[k]);
    }
    if (!m_logged || (draped && !m_loggedDrape))
    {
        m_logged = true;
        m_loggedDrape = m_loggedDrape || draped;
        Log::Write("waves: drawn with our wave shader (opacity over time where a wave model carries one; %s)",
                   draped ? "draped over the terrain" : "flat: no terrain heights yet, or no vertex textures");
    }
    return true;
}

void Waves::ReleaseAll()
{
    // No reference counting afterwards: this can be the device's last object.
    IUnknown* objects[] = {m_vs, m_ps, m_drapeVs, m_drapePs};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_vs = nullptr;
    m_ps = nullptr;
    m_drapeVs = nullptr;
    m_drapePs = nullptr;
    m_drapeTried = false;
    m_internalRefs = 0;
}
