#include "water.h"
#include "assets.h"
#include "camera.h"
#include "shadows.h"
#include "water_ps.h"
#include "water_surface_ps.h"
#include "water_surface_vs.h"
#include "water_vs.h"
#include "matrix.h"
#include "overlay.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// Length of a shader token stream within maxDwords (see ShaderSize in proxy_device.cpp), or 0.
size_t TokenStreamSize(const DWORD* f, size_t maxDwords)
{
    for (size_t i = 1; i < maxDwords;)
    {
        DWORD t = f[i];
        if (t == 0x0000FFFF)
            return (i + 1) * sizeof(DWORD);
        if ((t & 0xFFFF) == 0xFFFE)
            i += 1 + ((t >> 16) & 0x7FFF);
        else
            ++i;
    }
    return 0;
}

IDirect3DTexture9* SolidTexture(IDirect3DDevice9* dev, DWORD argb)
{
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0)))
    {
        *static_cast<DWORD*>(lr.pBits) = argb;
        t->UnlockRect(0);
    }
    return t;
}

const D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_ADDRESSW,
                                              D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
                                              D3DSAMP_SRGBTEXTURE};
const int kStages = 8;      // Colonization's water samplers are s0-s6
const int kVsSaved = 34;    // its vertex constants reach c33 (mtxShadow c30-c33)
const int kPsSaved = 4;     // c0 f4WaterConstants, c1 sun^2, c2 f4WaterShadow, c3 f3SkyLight
}

bool Water::Ready()
{
    if (!m_tried)
    {
        m_tried = true;
        m_ok = Load();
        if (!m_ok)
            ReleaseAll();
    }
    return m_ok;
}

bool Water::Load()
{
    // The textures, each from the first of: a running mod's Assets\Art\GraphicsUpgrade (loose or in its FPK archives),
    // the mod's GraphicsUpgrade folder, the GraphicsUpgrade folder next to the DLL, a Colonization install
    // (Assets\Art0.FPK, or loose files at the paths in its CIV4WaterPlaneInfos.xml). The normal map is water_normal.dds;
    // in the two GraphicsUpgrade folders also under its old name (Colonization's own), water_001.dds, as 1.0.x
    // installs and mods have it. The environment cube is water_env.dds. The shaders (water.hlsl, water_surface.hlsl)
    // are built into the DLL.
    struct Texture
    {
        const wchar_t *name, *oldName, *colonizationName;
        std::vector<uint8_t> bytes;
        std::wstring from;
    } textures[2] = {{L"water_normal.dds", L"water_001.dds", L"water_001.dds", {}, {}},
                     {L"water_env.dds", nullptr, L"water_env.dds", {}, {}}};
    auto find = [&](Texture& t) {
        if (ReadAsset(std::wstring(L"Art\\GraphicsUpgrade\\") + t.name, t.bytes, t.from, true))
            return true;
        for (const std::wstring& folder : {g_config.modGraphicsDir, ProxyDir() + L"\\GraphicsUpgrade"})
            for (const wchar_t* file : {t.name, t.oldName})
                if (!folder.empty() && file && ReadFileBytes(folder + L"\\" + file, t.bytes))
                {
                    t.from = folder + L"\\" + file;
                    return true;
                }
        const std::wstring& dir = g_config.colonizationDir;
        const std::wstring loose = std::wstring(L"Art\\Terrain\\Water\\RRWater\\") + t.colonizationName;
        std::string packed;
        for (wchar_t c : loose)
            packed += static_cast<char>(towlower(c));
        for (const std::wstring& path : {dir + L"\\Assets\\Art0.FPK", dir + L"\\Assets\\" + loose})
            if (path.size() > 4 && path.compare(path.size() - 4, 4, L".FPK") == 0 ? ReadFromFpk(path, packed, t.bytes)
                                                                                 : ReadFileBytes(path, t.bytes))
            {
                t.from = path;
                return true;
            }
        return false;
    };
    for (Texture& t : textures)
        if (!find(t))
        {
            Log::Write("water: textures not found: %ls isn't in the mod's Assets\\Art\\GraphicsUpgrade or GraphicsUpgrade "
                       "folder, %ls\\GraphicsUpgrade, or the Colonization install %ls; BtS water left as is",
                       t.name, ProxyDir().c_str(), g_config.colonizationDir.c_str());
            return false;
        }
    std::vector<uint8_t>& normalDds = textures[0].bytes;
    std::vector<uint8_t>& envDds = textures[1].bytes;
    const std::wstring& normalFrom = textures[0].from;
    const std::wstring& envFrom = textures[1].from;
    Log::Write("water: textures from %ls, %ls", normalFrom.c_str(), envFrom.c_str());
    const std::wstring source = normalFrom + L", " + envFrom;
    ULONG before = DeviceRefs(m_dev);
    std::vector<DWORD> vsCode(sizeof(g_waterVS) / 4), psCode(sizeof(g_waterPS) / 4);
    memcpy(vsCode.data(), g_waterVS, sizeof(g_waterVS));
    memcpy(psCode.data(), g_waterPS, sizeof(g_waterPS));
    std::vector<DWORD> surfaceVsCode(sizeof(g_waterSurfaceVS) / 4), surfacePsCode(sizeof(g_waterSurfacePS) / 4);
    memcpy(surfaceVsCode.data(), g_waterSurfaceVS, sizeof(g_waterSurfaceVS));
    memcpy(surfacePsCode.data(), g_waterSurfacePS, sizeof(g_waterSurfacePS));
    m_vsInfo.version = "vs_1_1";
    m_psInfo.version = "ps_2_0";
    m_vsInfo.hash = Fnv1a(vsCode.data(), sizeof(g_waterVS));
    m_psInfo.hash = Fnv1a(psCode.data(), sizeof(g_waterPS));
    ParseConstantTable(vsCode.data(), sizeof(g_waterVS), m_vsInfo);
    ParseConstantTable(psCode.data(), sizeof(g_waterPS), m_psInfo);
    if (FAILED(m_dev->CreateVertexShader(vsCode.data(), &m_vs)) || FAILED(m_dev->CreatePixelShader(psCode.data(), &m_ps)) ||
        FAILED(m_dev->CreateVertexShader(surfaceVsCode.data(), &m_surfaceVs)) ||
        FAILED(m_dev->CreatePixelShader(surfacePsCode.data(), &m_surfacePs)))
    {
        Log::Write("water: creating the water shaders failed; BtS water left as is");
        return false;
    }
    Log::Write("water: shaders vertex %s: %s; pixel %s: %s",
               Hex64(m_vsInfo.hash).c_str(), m_vsInfo.constants.c_str(), Hex64(m_psInfo.hash).c_str(),
               m_psInfo.constants.c_str());

    HMODULE d3dx = GetModuleHandleW(L"d3dx9_33.dll");
    if (!d3dx)
        d3dx = LoadLibraryW(L"d3dx9_33.dll");  // ships with both games
    using TexFn = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCVOID, UINT, IDirect3DTexture9**);
    using CubeFn = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCVOID, UINT, IDirect3DCubeTexture9**);
    auto texFromMem = d3dx ? reinterpret_cast<TexFn>(GetProcAddress(d3dx, "D3DXCreateTextureFromFileInMemory")) : nullptr;
    auto cubeFromMem =
        d3dx ? reinterpret_cast<CubeFn>(GetProcAddress(d3dx, "D3DXCreateCubeTextureFromFileInMemory")) : nullptr;
    if (!texFromMem || !cubeFromMem)
    {
        Log::Write("water: d3dx9_33.dll texture loaders not available; BtS water left as is");
        return false;
    }
    if (FAILED(texFromMem(m_dev, normalDds.data(), static_cast<UINT>(normalDds.size()), &m_normal)) ||
        FAILED(cubeFromMem(m_dev, envDds.data(), static_cast<UINT>(envDds.size()), &m_env)))
    {
        Log::Write("water: creating the water textures failed; BtS water left as is");
        return false;
    }
    m_black = SolidTexture(m_dev, 0xFF000000);
    m_clear = SolidTexture(m_dev, 0x00000000);
    if (!m_black || !m_clear)
        return false;
    m_internalRefs += DeviceRefs(m_dev) - before;
    m_startTick = GetTickCount();
    Log::Write("water: ready (normal map %zu bytes, environment cube %zu bytes, from %ls)",
               normalDds.size(), envDds.size(), source.c_str());
    return true;
}

bool Water::EnsureRefraction()
{
    if (m_refr)
        return true;
    ULONG before = DeviceRefs(m_dev);
    UINT size = g_config.reflectionSize;
    if (FAILED(m_dev->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &m_refr,
                                    nullptr)) ||
        FAILED(m_refr->GetSurfaceLevel(0, &m_refrSurf)))
    {
        if (m_refr)
            m_refr->Release();
        m_refr = nullptr;
        return false;
    }
    m_internalRefs += DeviceRefs(m_dev) - before;
    Log::Write("water: refraction texture %ux%u (screen copy) created", size, size);
    return true;
}

void Water::SnapshotRefraction()
{
    if (m_snapshotTaken || !Ready() || !EnsureRefraction())
        return;
    m_snapshotTaken = true;  // once per frame, even if the copy fails
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    HRESULT hr = m_dev->StretchRect(bb, nullptr, m_refrSurf, nullptr, D3DTEXF_LINEAR);
    bb->Release();
    static bool logged = false;
    if (FAILED(hr) && !logged)
    {
        logged = true;
        Log::Write("water: refraction StretchRect failed 0x%08x; water refracts black", static_cast<unsigned>(hr));
    }
}

bool Water::SaveRefractionBmp(const std::wstring& path)
{
    return m_refrSurf && SaveSurfaceBmp(m_dev, m_refrSurf, path);
}

bool Water::DrawSubmerged(const ShaderInfo* vs, float h, const std::function<void()>& redraw)
{
    const float depth = g_config.underwaterDepth, visibility = g_config.underwaterVisibility;
    const UINT bands = g_config.underwaterBands;
    if (!m_snapshotTaken || !m_refrSurf || depth <= 0 || visibility <= 0)
        return false;
    // Clip planes: fixed function takes them in world space; shaders in clip space, Plane_clip = Plane_world x M^-1.
    double inv[16];
    bool clipSpace = vs != nullptr;
    if (clipSpace)
    {
        float vp[16];
        if (!Camera::UnitWorldToClip(m_dev, *vs, vp) || !mat::Invert(vp, inv))
            return false;
    }
    auto toDevice = [&](const float* world, float* out) {
        if (!clipSpace)
        {
            memcpy(out, world, 4 * sizeof(float));
            return;
        }
        for (int c = 0; c < 4; ++c)
            out[c] = static_cast<float>(world[0] * inv[0 * 4 + c] + world[1] * inv[1 * 4 + c] +
                                        world[2] * inv[2 * 4 + c] + world[3] * inv[3 * 4 + c]);
    };

    SavedTarget saved;
    saved.Save(m_dev);
    const D3DRENDERSTATETYPE states[] = {D3DRS_ZENABLE,     D3DRS_ZWRITEENABLE,    D3DRS_ALPHABLENDENABLE,
                                         D3DRS_SRCBLEND,    D3DRS_DESTBLEND,       D3DRS_BLENDOP,
                                         D3DRS_BLENDFACTOR, D3DRS_CLIPPLANEENABLE, D3DRS_FOGENABLE,
                                         D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_COLORWRITEENABLE};
    const int n = sizeof(states) / sizeof(states[0]);
    DWORD old[n];
    for (int i = 0; i < n; ++i)
        m_dev->GetRenderState(states[i], &old[i]);
    float oldPlanes[2][4];
    m_dev->GetClipPlane(0, oldPlanes[0]);
    m_dev->GetClipPlane(1, oldPlanes[1]);

    // The refraction is a scaled screen copy without depth: no depth test (the hull's own back faces stay culled).
    m_dev->SetRenderTarget(0, m_refrSurf);
    m_dev->SetDepthStencilSurface(nullptr);
    m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR);
    m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR);
    m_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                      D3DCOLORWRITEENABLE_BLUE);
    m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0 | D3DCLIPPLANE1);
    // Band i: from h - i*d/bands down to h - (i+1)*d/bands, at visibility x (1 - (i + 0.5) / bands).
    for (UINT i = 0; i < bands; ++i)
    {
        float top = h - depth * i / bands, bottom = h - depth * (i + 1) / bands;
        const float below[4] = {0, 0, -1, top}, above[4] = {0, 0, 1, -bottom};  // z <= top, z >= bottom
        float plane[4];
        toDevice(below, plane);
        m_dev->SetClipPlane(0, plane);
        toDevice(above, plane);
        m_dev->SetClipPlane(1, plane);
        float a = visibility * (1.0f - (i + 0.5f) / bands);
        DWORD f = static_cast<DWORD>((std::min)(1.0f, (std::max)(0.0f, a)) * 255.0f + 0.5f);
        m_dev->SetRenderState(D3DRS_BLENDFACTOR, D3DCOLOR_ARGB(f, f, f, f));
        redraw();
    }

    m_dev->SetClipPlane(0, oldPlanes[0]);
    m_dev->SetClipPlane(1, oldPlanes[1]);
    for (int i = 0; i < n; ++i)
        m_dev->SetRenderState(states[i], old[i]);
    saved.Restore(m_dev);
    return true;
}

void Water::ReleaseDefault()
{
    m_snapshotTaken = false;
    // Only touch the device while we hold something: our objects keep it alive, but this also runs from the
    // destructor after the game has destroyed the device, when there's nothing left to release.
    if (!m_refr && !m_refrSurf)
        return;
    ULONG before = DeviceRefs(m_dev);
    if (m_refrSurf)
        m_refrSurf->Release();
    if (m_refr)
        m_refr->Release();
    m_refrSurf = nullptr;
    m_refr = nullptr;
    ULONG after = DeviceRefs(m_dev);
    m_internalRefs -= (before > after && m_internalRefs >= before - after) ? before - after : 0;
    m_snapshotTaken = false;
}

void Water::ReleaseAll()
{
    ReleaseDefault();
    IUnknown* objects[] = {m_vs, m_ps, m_surfaceVs, m_surfacePs, m_normal, m_env, m_black, m_clear};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_vs = nullptr;
    m_ps = nullptr;
    m_surfaceVs = nullptr;
    m_surfacePs = nullptr;
    m_normal = nullptr;
    m_env = nullptr;
    m_black = nullptr;
    m_clear = nullptr;
    m_internalRefs = 0;
}

HRESULT Water::Draw(const ShaderInfo& btsVs, const ShaderInfo* btsPs, IDirect3DTexture9* reflection,
                       float waterHeight, const std::function<HRESULT()>& draw)
{
    // --- BtS's inputs, read before anything is changed
    int wvpReg = btsVs.Reg("mtxWorldViewProj", 2);
    int worldReg = btsVs.world;
    float wvp[16], world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (wvpReg < 0 || worldReg < 0 || FAILED(m_dev->GetVertexShaderConstantF(wvpReg, wvp, 4)) ||
        FAILED(m_dev->GetVertexShaderConstantF(worldReg, world, 3)))
        return draw();  // not the BtS water we know: leave it alone
    int coastStage = btsPs && btsPs->Reg("CoastFade", 3) >= 0 ? btsPs->Reg("CoastFade", 3) : 2;
    int fogStage = btsPs && btsPs->Reg("Fog", 3) >= 0 ? btsPs->Reg("Fog", 3) : 3;
    IDirect3DBaseTexture9 *coast = nullptr, *fog = nullptr;
    m_dev->GetTexture(coastStage, &coast);
    m_dev->GetTexture(fogStage, &fog);

    // --- matrices: world -> clip VP = WVP * World^-1; eye from VP; mirrored VP for the reflection lookup
    float worldInv[16], vp[16], mirrored[16], reflTex[16], refrTex[16], eye[3] = {0, 0, 0};
    world[12] = world[13] = world[14] = 0;
    world[15] = 1;
    if (!mat::Invert(world, worldInv))
    {
        if (coast)
            coast->Release();
        if (fog)
            fog->Release();
        return draw();
    }
    mat::Mul(wvp, worldInv, vp);
    mat::EyePosition(vp, eye);
    mat::MirrorViewProj(vp, waterHeight, mirrored);
    mat::TextureProjection(mirrored, reflTex);
    mat::TextureProjection(vp, refrTex);

    // Normal-map layers: world xy * scale, scrolling at Colonization's URate/VRate (second layer mirrored in u and v).
    float t = (GetTickCount() - m_startTick) / 1000.0f;
    float su = g_config.waterURate * t, sv = g_config.waterVRate * t;
    su -= static_cast<float>(static_cast<int>(su));
    sv -= static_cast<float>(static_cast<int>(sv));
    const float s = g_config.waterTexScale;
    const float tex1[12] = {s, 0, 0, 0, 0, s, 0, 0, su, sv, 1, 0};
    const float tex2[12] = {-s, 0, 0, 0, 0, s, 0, 0, su, -sv, 1, 0};
    const float grid[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0.5f, 1, 0};  // grid overlay off (Colonization's default)
    const float invView[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, eye[0], eye[1], eye[2], 1};  // only row 3 is used

    // --- save BtS's state
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    m_dev->GetVertexShader(&oldVs);
    m_dev->GetPixelShader(&oldPs);
    float oldVsc[kVsSaved * 4], oldPsc[kPsSaved * 4];
    m_dev->GetVertexShaderConstantF(0, oldVsc, kVsSaved);
    m_dev->GetPixelShaderConstantF(0, oldPsc, kPsSaved);
    IDirect3DBaseTexture9* oldTex[kStages] = {};
    DWORD oldSamp[kStages][7];
    for (int st = 0; st < kStages; ++st)
    {
        m_dev->GetTexture(st, &oldTex[st]);
        for (int k = 0; k < 7; ++k)
            m_dev->GetSamplerState(st, kSamplerStates[k], &oldSamp[st][k]);
    }

    // --- Colonization's water
    auto setVs = [&](const char* name, const float* data, UINT regs) {
        int r = m_vsInfo.Reg(name, 2);
        if (r >= 0)
            m_dev->SetVertexShaderConstantF(r, data, regs);
    };
    setVs("mtxWorldViewProj", wvp, 4);
    setVs("mtxInvView", invView, 4);
    setVs("mtxWorld", world, 4);
    setVs("mtxReflection", reflTex, 4);
    setVs("mtxRefraction", refrTex, 4);
    setVs("mtxWaterTexture1", tex1, 3);
    setVs("mtxWaterTexture2", tex2, 3);
    setVs("mtxWaterGrid", grid, 3);
    int wc = m_psInfo.Reg("f4WaterConstants", 2);
    m_dev->SetPixelShaderConstantF(wc >= 0 ? wc : 0, g_config.waterConstants, 1);
    // c1 = f3SunLightDiffuse^2 (the effect's preshader works it out; the shader is in roughly gamma-2 space): the
    // lighting's Water sun, or Colonization's own sun (0.999, 0.920647, 0.736518).
    static const float kColonizationSun[3] = {0.999f, 0.920647f, 0.736518f};
    const float* sunRgb = sunColour ? sunColour : kColonizationSun;
    const float sun[4] = {sunRgb[0] * sunRgb[0], sunRgb[1] * sunRgb[1], sunRgb[2] * sunRgb[2], 0};
    m_dev->SetPixelShaderConstantF(1, sun, 1);
    // Sun shadows on the surface and the sky light (GraphicsUpgrade's additions to the shader).
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    setVs("mtxShadow", shadowTexture && shadowMatrix ? shadowMatrix : kIdentity, 4);
    const float waterShadow[4] = {shadowTexture ? (std::min)(1.0f, (std::max)(0.0f, g_config.waterShadow)) : 0.0f,
                                  shadowTexture ? 1.0f : 0.0f, shadowDarkness, 0};
    static const float kNoTint[3] = {1, 1, 1};
    const float* tint = skyLight ? skyLight : kNoTint;
    const float sky[4] = {tint[0], tint[1], tint[2], 0};
    int wsReg = m_psInfo.Reg("f4WaterShadow", 2), skyReg = m_psInfo.Reg("f3SkyLight", 2);
    if (wsReg >= 0)
        m_dev->SetPixelShaderConstantF(wsReg, waterShadow, 1);
    if (skyReg >= 0)
        m_dev->SetPixelShaderConstantF(skyReg, sky, 1);
    if (!m_loggedSun)
    {
        m_loggedSun = true;
        Log::Write("water: sun %.0f,%.0f,%.0f (%s), sky tint %.2f,%.2f,%.2f", sunRgb[0] * 255, sunRgb[1] * 255,
                   sunRgb[2] * 255, sunColour ? "the lighting's" : "Colonization's", tint[0], tint[1], tint[2]);
    }

    // Sampler setup as Colonization's water draw has it: address, filters, sRGB read.
    auto bind = [&](const char* name, IDirect3DBaseTexture9* tex, DWORD address, DWORD mip, DWORD srgb) {
        int st = m_psInfo.Reg(name, 3);
        if (st < 0)
            return;
        m_dev->SetTexture(st, tex);
        m_dev->SetSamplerState(st, D3DSAMP_ADDRESSU, address);
        m_dev->SetSamplerState(st, D3DSAMP_ADDRESSV, address);
        m_dev->SetSamplerState(st, D3DSAMP_ADDRESSW, address);
        m_dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_dev->SetSamplerState(st, D3DSAMP_MIPFILTER, mip);
        m_dev->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, srgb);
    };
    bind("NormalMap", m_normal, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, FALSE);
    bind("CoastFade", coast, D3DTADDRESS_WRAP, D3DTEXF_NONE, FALSE);
    bind("FogSRGB", fog, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, TRUE);
    bind("GridMap", m_black, D3DTADDRESS_WRAP, D3DTEXF_NONE, FALSE);
    bind("EnvironmentMap", m_env, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, TRUE);
    bind("ReflectionMap", reflection ? static_cast<IDirect3DBaseTexture9*>(reflection) : m_clear, D3DTADDRESS_CLAMP,
         D3DTEXF_NONE, TRUE);
    bind("RefractionMap", m_snapshotTaken && m_refr ? static_cast<IDirect3DBaseTexture9*>(m_refr) : m_black,
         D3DTADDRESS_CLAMP, D3DTEXF_NONE, TRUE);
    int shadowStage = m_psInfo.Reg("ShadowMap", 3);
    if (shadowStage >= 0)
    {
        m_dev->SetTexture(shadowStage, shadowTexture);
        Shadows::BindSampler(m_dev, shadowStage);
    }
    m_dev->SetVertexShader(m_vs);
    m_dev->SetPixelShader(m_ps);

    HRESULT hr = draw();

    // --- restore BtS's state
    m_dev->SetVertexShader(oldVs);
    m_dev->SetPixelShader(oldPs);
    m_dev->SetVertexShaderConstantF(0, oldVsc, kVsSaved);
    m_dev->SetPixelShaderConstantF(0, oldPsc, kPsSaved);
    for (int st = 0; st < kStages; ++st)
    {
        m_dev->SetTexture(st, oldTex[st]);
        for (int k = 0; k < 7; ++k)
            m_dev->SetSamplerState(st, kSamplerStates[k], oldSamp[st][k]);
        if (oldTex[st])
            oldTex[st]->Release();
    }
    if (oldVs)
        oldVs->Release();
    if (oldPs)
        oldPs->Release();
    if (coast)
        coast->Release();
    if (fog)
        fog->Release();
    return hr;
}

bool Water::DrawSurface(const std::function<HRESULT()>& draw)
{
    if (!m_surfaceVs || !m_surfacePs)
        return false;
    // --- the mesh as fixed function draws it: xyz positions, stage 0 reading texture coordinate set 0 (2D),
    // transformed by a 2D texture matrix or not at all
    DWORD fvf = 0, tci = 0, ttf = 0;
    m_dev->GetFVF(&fvf);
    m_dev->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &tci);
    m_dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
    const DWORD sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || sets < 1 || ((fvf >> 16) & 3) != D3DFVF_TEXTUREFORMAT2 ||
        tci != 0 || (ttf != D3DTTFF_DISABLE && ttf != D3DTTFF_COUNT2))
    {
        if (!m_loggedSurface)
            Log::Write("water: a SurfaceTextures mesh drawn in a way the surface water can't follow (fvf 0x%x, "
                       "texcoord index 0x%x, transform flags 0x%x); left as BtS draws it",
                       static_cast<unsigned>(fvf), static_cast<unsigned>(tci), static_cast<unsigned>(ttf));
        m_loggedSurface = true;
        return false;
    }
    D3DMATRIX w, v, p, t;
    m_dev->GetTransform(D3DTS_WORLD, &w);
    m_dev->GetTransform(D3DTS_VIEW, &v);
    m_dev->GetTransform(D3DTS_PROJECTION, &p);
    m_dev->GetTransform(D3DTS_TEXTURE0, &t);
    // Fixed function multiplies row vectors (v x M); the shader's registers hold the transposes ("register form").
    auto registers = [](const D3DMATRIX& m, float* out) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r * 4 + c] = m.m[c][r];
    };
    float world[16], view[16], proj[16], viewProj[16], wvp[16], viewInv[16];
    registers(w, world);
    registers(v, view);
    registers(p, proj);
    mat::Mul(proj, view, viewProj);
    mat::Mul(viewProj, world, wvp);
    if (!mat::Invert(&v._11, viewInv))
        return false;
    const float eye[4] = {viewInv[12], viewInv[13], viewInv[14], 1};  // the camera: the inverse view's last row
    // 2D texture coordinates are transformed as (u, v, 1): rows 1, 2 and 3 of the texture matrix.
    const float base[12] = {t._11, t._12, 0, 0, t._21, t._22, 0, 0, t._31, t._32, 1, 0};
    static const float kNoTransform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // Its alpha as fixed function works it out: the texture's, times the diffuse alpha (the vertex colour's, or the
    // material's when lit) unless stage 0 takes the texture's alone.
    DWORD lighting = FALSE, colorVertex = TRUE, diffuseSource = D3DMCS_COLOR1, alphaOp = D3DTOP_MODULATE,
          alphaArg = D3DTA_TEXTURE;
    m_dev->GetRenderState(D3DRS_LIGHTING, &lighting);
    m_dev->GetRenderState(D3DRS_COLORVERTEX, &colorVertex);
    m_dev->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &diffuseSource);
    m_dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &alphaOp);
    m_dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &alphaArg);
    D3DMATERIAL9 material = {};
    m_dev->GetMaterial(&material);
    const bool textureOnly = alphaOp == D3DTOP_SELECTARG1 && (alphaArg & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
    const bool vertexAlpha = !textureOnly && (fvf & D3DFVF_DIFFUSE) &&
                             (!lighting || (colorVertex && diffuseSource == D3DMCS_COLOR1));
    const float alpha[4] = {vertexAlpha ? 1.0f : 0.0f, textureOnly || !lighting ? 1.0f : material.Diffuse.a, 0, 0};

    float time = (GetTickCount() - m_startTick) / 1000.0f;
    float su = g_config.waterURate * time, sv = g_config.waterVRate * time;
    su -= static_cast<float>(static_cast<int>(su));
    sv -= static_cast<float>(static_cast<int>(sv));
    const float s = g_config.waterTexScale;
    const float tex1[12] = {s, 0, 0, 0, 0, s, 0, 0, su, sv, 1, 0};
    const float tex2[12] = {-s, 0, 0, 0, 0, s, 0, 0, su, -sv, 1, 0};
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const float kColonizationSun[3] = {0.999f, 0.920647f, 0.736518f};
    const float* sunRgb = sunColour ? sunColour : kColonizationSun;
    const float sun[4] = {sunRgb[0] * sunRgb[0], sunRgb[1] * sunRgb[1], sunRgb[2] * sunRgb[2], 0};
    const float waterShadow[4] = {0, shadowTexture ? 1.0f : 0.0f, shadowDarkness, 0};
    static const float kNoTint[3] = {1, 1, 1};
    const float* tint = skyLight ? skyLight : kNoTint;
    const float sky[4] = {tint[0], tint[1], tint[2], 0};

    // --- save BtS's state
    const int kSurfaceVs = 23, kSurfacePs = 4, kSurfaceStages = 4;
    IDirect3DBaseTexture9* baseTexture = nullptr;
    m_dev->GetTexture(0, &baseTexture);
    DWORD baseAddress[2] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP};
    m_dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &baseAddress[0]);
    m_dev->GetSamplerState(0, D3DSAMP_ADDRESSV, &baseAddress[1]);
    float oldVsc[kSurfaceVs * 4], oldPsc[kSurfacePs * 4];
    m_dev->GetVertexShaderConstantF(0, oldVsc, kSurfaceVs);
    m_dev->GetPixelShaderConstantF(0, oldPsc, kSurfacePs);
    IDirect3DBaseTexture9* oldTex[kSurfaceStages] = {};
    DWORD oldSamp[kSurfaceStages][7];
    for (int st = 0; st < kSurfaceStages; ++st)
    {
        m_dev->GetTexture(st, &oldTex[st]);
        for (int k = 0; k < 7; ++k)
            m_dev->GetSamplerState(st, kSamplerStates[k], &oldSamp[st][k]);
    }
    const D3DRENDERSTATETYPE states[] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,         D3DRS_DESTBLEND,
                                         D3DRS_BLENDOP,          D3DRS_ZENABLE,          D3DRS_ZWRITEENABLE,
                                         D3DRS_ZFUNC,            D3DRS_DEPTHBIAS,        D3DRS_SLOPESCALEDEPTHBIAS,
                                         D3DRS_ALPHATESTENABLE,  D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE,
                                         D3DRS_SRGBWRITEENABLE,  D3DRS_SEPARATEALPHABLENDENABLE};
    const int n = sizeof(states) / sizeof(states[0]);
    DWORD oldStates[n];
    for (int k = 0; k < n; ++k)
        m_dev->GetRenderState(states[k], &oldStates[k]);

    // --- the water on top
    m_dev->SetVertexShaderConstantF(0, wvp, 4);
    m_dev->SetVertexShaderConstantF(4, world, 4);
    m_dev->SetVertexShaderConstantF(8, tex1, 3);
    m_dev->SetVertexShaderConstantF(11, tex2, 3);
    m_dev->SetVertexShaderConstantF(14, ttf == D3DTTFF_COUNT2 ? base : kNoTransform, 3);
    m_dev->SetVertexShaderConstantF(17, shadowTexture && shadowMatrix ? shadowMatrix : kIdentity, 4);
    m_dev->SetVertexShaderConstantF(21, eye, 1);
    m_dev->SetVertexShaderConstantF(22, alpha, 1);
    m_dev->SetPixelShaderConstantF(0, g_config.waterConstants, 1);
    m_dev->SetPixelShaderConstantF(1, sun, 1);
    m_dev->SetPixelShaderConstantF(2, waterShadow, 1);
    m_dev->SetPixelShaderConstantF(3, sky, 1);
    auto bind = [&](DWORD st, IDirect3DBaseTexture9* tex, DWORD u, DWORD v, DWORD mip, DWORD srgb) {
        m_dev->SetTexture(st, tex);
        m_dev->SetSamplerState(st, D3DSAMP_ADDRESSU, u);
        m_dev->SetSamplerState(st, D3DSAMP_ADDRESSV, v);
        m_dev->SetSamplerState(st, D3DSAMP_ADDRESSW, u);
        m_dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_dev->SetSamplerState(st, D3DSAMP_MIPFILTER, mip);
        m_dev->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, srgb);
    };
    bind(0, m_normal, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, FALSE);
    bind(1, baseTexture, baseAddress[0], baseAddress[1], D3DTEXF_LINEAR, FALSE);
    bind(2, m_env, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, TRUE);
    m_dev->SetTexture(3, shadowTexture);
    Shadows::BindSampler(m_dev, 3);
    // Premultiplied: what's there x (1 - k) + sky x k + glint. The same surface again, a hair nearer (fixed function
    // and the shader may round its depth differently).
    const float bias = -0.00001f, noSlope = 0;
    DWORD biasBits, noSlopeBits;
    memcpy(&biasBits, &bias, 4);
    memcpy(&noSlopeBits, &noSlope, 4);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    m_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    m_dev->SetRenderState(D3DRS_DEPTHBIAS, biasBits);
    m_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, noSlopeBits);
    m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                      D3DCOLORWRITEENABLE_BLUE);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    m_dev->SetVertexShader(m_surfaceVs);
    m_dev->SetPixelShader(m_surfacePs);

    draw();

    // --- BtS's state back
    m_dev->SetVertexShader(nullptr);
    m_dev->SetPixelShader(nullptr);
    for (int k = 0; k < n; ++k)
        m_dev->SetRenderState(states[k], oldStates[k]);
    m_dev->SetVertexShaderConstantF(0, oldVsc, kSurfaceVs);
    m_dev->SetPixelShaderConstantF(0, oldPsc, kSurfacePs);
    for (int st = 0; st < kSurfaceStages; ++st)
    {
        m_dev->SetTexture(st, oldTex[st]);
        for (int k = 0; k < 7; ++k)
            m_dev->SetSamplerState(st, kSamplerStates[k], oldSamp[st][k]);
        if (oldTex[st])
            oldTex[st]->Release();
    }
    if (baseTexture)
        baseTexture->Release();
    if (!m_loggedSurface)
        Log::Write("water: water on a model (SurfaceTextures) drawn with the surface water (fvf 0x%x, %s alpha)",
                   static_cast<unsigned>(fvf), vertexAlpha ? "vertex" : textureOnly ? "texture" : "material");
    m_loggedSurface = true;
    return true;
}
