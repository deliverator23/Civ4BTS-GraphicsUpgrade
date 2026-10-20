#include "water.h"
#include "assets.h"
#include "camera.h"
#include "shadows.h"
#include "water_ps.h"
#include "water_shore_ps.h"
#include "water_shore_vs.h"
#include "water_surface_ps.h"
#include "water_surface_vs.h"
#include "water_vs.h"
#include "matrix.h"
#include "overlay.h"

#include <cctype>
#include <cmath>
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
const int kStages = 12;     // Colonization's water samplers are s0-s6; the shadow s7; the shoreline's s8-s11
const int kVsSaved = 36;    // its vertex constants reach c33 (mtxShadow c30-c33); the shoreline's c34-c35
const int kPsSaved = 10;    // c0 f4WaterConstants, c1 sun^2, c2 f4WaterShadow, c3 f3SkyLight; the shoreline's c4-c7;
                            // c8 f4NormalFormat; c9 the shoreline's f4WaveLight

// The sun painted into the sky's alpha (Colonization's water_env.dds as GraphicsUpgrade ships it): bearing 38.2
// degrees, 42.9 degrees up (unit vector, z up, y north). The glint is turned from the sun now onto this.
const float kPaintedSun[3] = {0.4531f, 0.5757f, 0.6807f};
// GlintFollowsSun=1: the edge of the view kGlintAcross radians to the side, kGlintOff of them just out of it (the
// glint faded out); kGlintBack of each crossing at each end is spent going round the back.
const float kGlintAcross = 28 * 3.14159265f / 180, kGlintOff = 1.25f * kGlintAcross, kGlintBack = 0.03f;
// World units per repeat of water_normal_1.dds, _2... (8 plots); water_normal.dds keeps Colonization's TextureScaling.
const float kNumberedRepeat = 1440.0f;

// A normal map stored as DXT5nm: x in alpha, y in green (z rebuilt in the shader), which DXT5 keeps far better than
// three colour channels. Marked as NVIDIA's tools mark it: DDPF_NORMAL in the DDS pixel format's flags.
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
    m_normalNm = IsDxt5nm(normalDds);
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
    // More normal maps to cycle through: water_normal_1.dds, _2... up to the first number missing, each from the
    // first of the places water_normal.dds is looked for (a Colonization install has none).
    for (int i = 1; i <= 32; ++i)
    {
        const std::wstring name = L"water_normal_" + std::to_wstring(i) + L".dds";
        std::vector<uint8_t> bytes;
        std::wstring from;
        if (!ReadAsset(L"Art\\GraphicsUpgrade\\" + name, bytes, from, true))
            for (const std::wstring& folder : {g_config.modGraphicsDir, ProxyDir() + L"\\GraphicsUpgrade"})
                if (!folder.empty() && ReadFileBytes(folder + L"\\" + name, bytes))
                {
                    from = folder + L"\\" + name;
                    break;
                }
        if (from.empty())
            break;
        IDirect3DTexture9* tex = nullptr;
        if (FAILED(texFromMem(m_dev, bytes.data(), static_cast<UINT>(bytes.size()), &tex)))
        {
            Log::Write("water: %ls can't be loaded; normal maps after it left out", from.c_str());
            break;
        }
        m_numbered.push_back(tex);
        m_numberedFrom.push_back(from);
        m_numberedNm.push_back(IsDxt5nm(bytes));
        Log::Write("water: normal map %d: %ls (%g world units per repeat%s)", i, from.c_str(), kNumberedRepeat,
                   m_numberedNm.back() ? ", DXT5nm" : "");
    }
    m_normalMap = static_cast<size_t>((std::max)(0, g_config.waterNormalMap));
    if (m_normalMap >= NormalMapCount())
    {
        Log::Write("water: [water] NormalMap=%d, but there's no water_normal_%d.dds; water_normal.dds instead",
                   g_config.waterNormalMap, g_config.waterNormalMap);
        m_normalMap = 0;
    }
    else if (m_normalMap)
        Log::Write("water: starting with normal map %zu", m_normalMap);
    m_black = SolidTexture(m_dev, 0xFF000000);
    m_clear = SolidTexture(m_dev, 0x00000000);
    if (!m_black || !m_clear)
        return false;
    LoadShore([&](const std::vector<uint8_t>& dds, IDirect3DTexture9** out) {
        return SUCCEEDED(texFromMem(m_dev, dds.data(), static_cast<UINT>(dds.size()), out));
    });
    m_internalRefs += DeviceRefs(m_dev) - before;
    m_startTick = GetTickCount();
    Log::Write("water: ready (normal map %zu bytes%s, environment cube %zu bytes, from %ls); reflection evenness %g, "
               "reflection ripple %g, refraction ripple %g", normalDds.size(), m_normalNm ? " DXT5nm" : "",
               envDds.size(), source.c_str(), g_config.waterConstants[0], g_config.waterConstants[1],
               g_config.waterConstants[2]);
    return true;
}

void Water::NextNormalMap()
{
    if (!m_ok)
        return;  // nothing loaded yet (no water drawn so far)
    if (NormalMapCount() < 2)
    {
        Log::Write("water: only water_normal.dds; add water_normal_1.dds, _2... beside it to cycle through");
        return;
    }
    m_normalMap = (m_normalMap + 1) % NormalMapCount();
    Log::Write("water: normal map %zu of %zu: %ls", m_normalMap, NormalMapCount() - 1,
               m_normalMap ? m_numberedFrom[m_normalMap - 1].c_str() : L"water_normal.dds (Colonization's tiling)");
}

float Water::GlintAcross() const
{
    // By day from sunrise (a quarter of the cycle before midday) to sunset; at night the same from sunset to sunrise.
    const float f = dayFraction - std::floor(dayFraction);
    return f >= 0.25f && f < 0.75f ? (f - 0.25f) * 2 : std::fmod(f + 0.25f, 1.0f) * 2;
}

float Water::GlintSide() const
{
    // Once round per crossing, with no jump: at a steady pace across the view and on until the glint has faded out
    // (kGlintOff), so it never speeds up in sight; then, out of view, speeding up smoothly (from that same pace) round
    // the back, where it meets the next crossing coming the other way round at the same speed.
    const float kPi = 3.14159265f, u = GlintAcross() - 0.5f, across = 0.5f - kGlintBack;
    const float pace = kGlintOff / across, a = std::fabs(u);
    float side = pace * a;
    if (a > across)
    {
        const float s = (std::min)(1.0f, (a - across) / kGlintBack), v = pace * kGlintBack;
        side = kGlintOff + v * s + (kPi - kGlintOff - v) * s * s;
    }
    return u < 0 ? side : -side;
}

float Water::GlintLow() const
{
    // With the side squared, the glint's path on the screen is an arc: over the middle, down to each edge (and it stays
    // down round the back).
    const float s = GlintSide() / kGlintAcross;
    return (std::min)(1.0f, s * s);
}

float Water::GlintFade() const
{
    // GlintFollowsSun=1 with the light cycle: the glint dims as it nears the edge of the view and is gone just out of
    // it, so whatever of it is still in view as it slips off one edge dims away instead of vanishing at once.
    if (g_config.glintFollowsSun != 1 || dayFraction < 0)
        return 1.0f;
    auto smooth = [](float x) { x = (std::min)(1.0f, (std::max)(0.0f, x)); return x * x * (3 - 2 * x); };
    return smooth((kGlintOff - std::fabs(GlintSide())) / (kGlintOff - 0.8f * kGlintAcross));
}

void Water::GlintLight(const float vp[16], float toLight[4]) const
{
    toLight[3] = GlintFade();
    if (!(g_config.glintFollowsSun > 0 && sunDirection))
    {
        memcpy(toLight, kPaintedSun, sizeof(kPaintedSun));
        return;
    }
    float toSun[3] = {-sunDirection[0], -sunDirection[1], (std::max)(0.05f, -sunDirection[2])};
    // GlintFollowsSun=1: the glint kept in view, crossing it like the sun and moon. The camera's heading and
    // tilt: where clip w (row 4 of the view-projection) grows. While the light cycle runs, the glint goes round
    // the camera once by day and once by night, right to left, smoothly (no jumps, so neither does the wave
    // shading's light): steadily across the view, the middle at midday (the moon's at midnight), and quickly round
    // the back, behind the camera at sunset and sunrise, where the sun's glint hands over to the moon's. On the
    // screen it arcs like the sun across the sky: up from low at the right edge to above the middle, then down
    // to low at the left edge (a light 8 degrees above the camera's tilt in the middle, 15 below it at the
    // edges; the glint is the light's mirror image, so a lower light puts it further off, higher on the
    // screen). With no cycle (a fixed sun): how far the sun is to the side of the camera's heading (the sine of
    // the angle, so a sun behind the camera counts as in front), the right edge for a sun due east, the left due
    // west; its height from the sun's, 0 to 60 degrees up narrowed to 15 degrees either side of the camera's
    // tilt (a low sun: near the top of the view). Without a perspective view (no heading to go by) the sun is
    // left as it is.
    const float flat = std::sqrt(vp[12] * vp[12] + vp[13] * vp[13]);
    if (g_config.glintFollowsSun == 1 && flat > 1e-6f)
    {
        const float kPi = 3.14159265f, kDeg = kPi / 180, kHalfBand = 15 * kDeg;
        const float ahead[2] = {vp[12] / flat, vp[13] / flat}, right[2] = {ahead[1], -ahead[0]};
        const float tilt = std::atan2(-vp[14], flat);  // how far the camera looks down
        float side, up;
        if (dayFraction >= 0)
        {
            side = GlintSide();
            up = tilt - 8 * kDeg + 23 * kDeg * GlintLow();
        }
        else
        {
            const float sideways = toSun[0] * right[0] + toSun[1] * right[1];
            const float level0 = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1]);
            side = kGlintAcross * (level0 > 1e-6f ? sideways / level0 : 0.0f);
            const float high = (std::min)(1.0f, std::atan2(toSun[2], level0) / (60 * kDeg));
            up = tilt - kHalfBand + 2 * kHalfBand * high;
        }
        up = (std::min)(80 * kDeg, (std::max)(10 * kDeg, up));
        const float level = std::cos(up);
        toSun[0] = level * (std::sin(side) * right[0] + std::cos(side) * ahead[0]);
        toSun[1] = level * (std::sin(side) * right[1] + std::cos(side) * ahead[1]);
        toSun[2] = std::sin(up);
    }
    const float len = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
    for (float& v : toSun)
        v /= len > 0 ? len : 1;
    memcpy(toLight, toSun, sizeof(toSun));
}

void Water::GlintTurn(const float toSun[3], float rot[12])
{
    // The rotation R with R toSun = the painted sun, as rows (the shader turns a direction v by v x mtxGlint).
    const float* p = kPaintedSun;
    const float a[3] = {toSun[1] * p[2] - toSun[2] * p[1], toSun[2] * p[0] - toSun[0] * p[2], toSun[0] * p[1] - toSun[1] * p[0]};
    const float c = toSun[0] * p[0] + toSun[1] * p[1] + toSun[2] * p[2];
    if (c <= -0.999f)
        return;
    // Rodrigues: R = I + [a]x + [a]x^2 / (1 + c)
    const float k = 1.0f / (1.0f + c);
    const float m[3][3] = {{1 - k * (a[1] * a[1] + a[2] * a[2]), -a[2] + k * a[0] * a[1], a[1] + k * a[0] * a[2]},
                           {a[2] + k * a[0] * a[1], 1 - k * (a[0] * a[0] + a[2] * a[2]), -a[0] + k * a[1] * a[2]},
                           {-a[1] + k * a[0] * a[2], a[0] + k * a[1] * a[2], 1 - k * (a[0] * a[0] + a[1] * a[1])}};
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            rot[r * 4 + col] = m[r][col];
}

IDirect3DBaseTexture9* Water::GlintSky(const float vp[16], float rot[12], float& fade)
{
    // As Draw: the painted sun as it is without a sun to follow (GlintFollowsSun=0), else turned onto the glint's.
    static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(rot, kIdentity, sizeof(kIdentity));
    fade = 1;
    if (!Ready() || !m_env)
        return nullptr;
    if (g_config.glintFollowsSun > 0 && sunDirection)
    {
        float toSun[4];
        GlintLight(vp, toSun);
        GlintTurn(toSun, rot);
        fade = toSun[3];
    }
    return m_env;
}

void Water::NormalLayers(float time, float tex1[12], float tex2[12]) const
{
    // World xy x scale, scrolling at Colonization's URate/VRate (second layer mirrored in u and v). The numbered maps
    // cover kNumberedRepeat world units, so their scroll (in repeats) is slowed by as much: the same speed on the map.
    const float s = m_normalMap ? 1.0f / kNumberedRepeat : g_config.waterTexScale;
    const float speed = s / g_config.waterTexScale;
    float su = g_config.waterURate * speed * time, sv = g_config.waterVRate * speed * time;
    su -= static_cast<float>(static_cast<int>(su));
    sv -= static_cast<float>(static_cast<int>(sv));
    const float a[12] = {s, 0, 0, 0, 0, s, 0, 0, su, sv, 1, 0};
    const float b[12] = {-s, 0, 0, 0, 0, s, 0, 0, su, -sv, 1, 0};
    memcpy(tex1, a, sizeof(a));
    memcpy(tex2, b, sizeof(b));
}

void Water::LoadShore(const std::function<bool(const std::vector<uint8_t>&, IDirect3DTexture9**)>& texture)
{
    // Shader Model 3 shaders, if the card has it and something uses them.
    const bool glint = g_config.glintFollowsSun > 0, shore = g_config.caustics > 0, waves = g_config.waveShading > 0;
    D3DCAPS9 caps = {};
    if ((!glint && !shore && !waves) || FAILED(m_dev->GetDeviceCaps(&caps)))
        return;
    if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
    {
        Log::Write("water: the shoreline and shallow water need Shader Model 3; left out");
        return;
    }
    std::vector<DWORD> vsCode(sizeof(g_waterShoreVS) / 4), psCode(sizeof(g_waterShorePS) / 4);
    memcpy(vsCode.data(), g_waterShoreVS, sizeof(g_waterShoreVS));
    memcpy(psCode.data(), g_waterShorePS, sizeof(g_waterShorePS));
    m_shoreVsInfo.version = "vs_3_0";
    m_shorePsInfo.version = "ps_3_0";
    ParseConstantTable(vsCode.data(), sizeof(g_waterShoreVS), m_shoreVsInfo);
    ParseConstantTable(psCode.data(), sizeof(g_waterShorePS), m_shorePsInfo);
    if (FAILED(m_dev->CreateVertexShader(vsCode.data(), &m_shoreVs)) || FAILED(m_dev->CreatePixelShader(psCode.data(), &m_shorePs)))
    {
        if (m_shoreVs)
            m_shoreVs->Release();
        m_shoreVs = nullptr;
        m_shorePs = nullptr;
        Log::Write("water: creating the shoreline water shaders failed; left out");
        return;
    }
    if (!shore)
    {
        Log::Write("water: %s (caustics off)", glint ? "the glint follows the sun" : "wave shading");
        return;
    }
    // Civilization V's caustic networks: from the GraphicsUpgrade folders (the mod's, then the game's), else
    // Civilization V's own archive.
    struct Source
    {
        const wchar_t* name;
        const char* civ5Name;
        IDirect3DTexture9** out;
    } sources[2] = {{L"water_caustics1.dds", "fx_caustics_base_01.dds", &m_caustic[0]},
                    {L"water_caustics2.dds", "fx_caustics_base_02.dds", &m_caustic[1]}};
    const std::wstring civ5Pack = g_config.civ5Dir + L"\\Resource\\DX9\\EffectTextures.fpk";
    std::wstring from;
    for (Source& s : sources)
    {
        std::vector<uint8_t> dds;
        bool found = false;
        for (const std::wstring& folder : {g_config.modGraphicsDir, ProxyDir() + L"\\GraphicsUpgrade"})
            if (!found && !folder.empty() && ReadFileBytes(folder + L"\\" + s.name, dds))
                found = true, from = folder;
        if (!found && ReadFromCiv5Fpk(civ5Pack, s.civ5Name, dds))
            found = true, from = civ5Pack;
        if (!found || !texture(dds, s.out))
        {
            Log::Write("water: %ls not found in the GraphicsUpgrade folder or %ls (or not a texture): no caustics%s",
                       s.name, civ5Pack.c_str(), glint ? "; the glint still follows the sun" : "");
            for (IDirect3DTexture9** t : {&m_caustic[0], &m_caustic[1]})
                if (*t)
                    (*t)->Release(), *t = nullptr;
            return;
        }
    }
    Log::Write("water: shoreline and shallow water ready (glint follows the sun %s, caustics %.2f; "
               "textures from %ls)",
               glint ? "on" : "off", g_config.caustics, from.c_str());
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
    // What the map is being drawn into: the back buffer, or the screen-sized texture behind the Civilopedia.
    IDirect3DSurface9* target = nullptr;
    if (FAILED(m_dev->GetRenderTarget(0, &target)) || !target)
        return;
    HRESULT hr = m_dev->StretchRect(target, nullptr, m_refrSurf, nullptr, D3DTEXF_LINEAR);
    target->Release();
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
    IUnknown* objects[] = {m_vs,     m_ps,         m_surfaceVs,  m_surfacePs, m_normal, m_env, m_black, m_clear,
                           m_shoreVs, m_shorePs,   m_caustic[0], m_caustic[1]};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    for (IDirect3DTexture9* t : m_numbered)
        t->Release();
    m_numbered.clear();
    m_numberedNm.clear();
    m_normalNm = false;
    m_numberedFrom.clear();
    m_normalMap = 0;
    m_vs = nullptr;
    m_ps = nullptr;
    m_surfaceVs = nullptr;
    m_surfacePs = nullptr;
    m_normal = nullptr;
    m_env = nullptr;
    m_black = nullptr;
    m_clear = nullptr;
    m_shoreVs = nullptr;
    m_shorePs = nullptr;
    m_caustic[0] = m_caustic[1] = nullptr;
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

    // Normal-map layers (NormalLayers).
    float t = g_config.waterTime >= 0 ? g_config.waterTime : (GetTickCount() - m_startTick) / 1000.0f;
    float tex1[12], tex2[12];
    NormalLayers(t, tex1, tex2);
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

    // --- Colonization's water, or the same with the shoreline (the glint following the sun; caustics where
    // the shore is known)
    const bool shoreReady = ShoreWanted() && shoreTexture;
    const bool glint = g_config.glintFollowsSun > 0 && sunDirection;
    const bool useShore = m_shoreVs && m_shorePs && (glint || shoreReady || g_config.waveShading > 0);
    const ShaderInfo& vsInfo = useShore ? m_shoreVsInfo : m_vsInfo;
    const ShaderInfo& psInfo = useShore ? m_shorePsInfo : m_psInfo;
    auto setVs = [&](const char* name, const float* data, UINT regs) {
        int r = vsInfo.Reg(name, 2);
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
    int wc = psInfo.Reg("f4WaterConstants", 2);
    m_dev->SetPixelShaderConstantF(wc >= 0 ? wc : 0, g_config.waterConstants, 1);
    const float normalFormat[4] = {CurrentNormalNm() ? 1.0f : 0.0f, 0, 0, 0};
    int nf = psInfo.Reg("f4NormalFormat", 2);
    if (nf >= 0)
        m_dev->SetPixelShaderConstantF(nf, normalFormat, 1);
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
    int wsReg = psInfo.Reg("f4WaterShadow", 2), skyReg = psInfo.Reg("f3SkyLight", 2);
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
        int st = psInfo.Reg(name, 3);
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
    bind("NormalMap", CurrentNormal(), D3DTADDRESS_WRAP, D3DTEXF_LINEAR, FALSE);
    bind("CoastFade", coast, D3DTADDRESS_WRAP, D3DTEXF_NONE, FALSE);
    bind("FogSRGB", fog, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, TRUE);
    bind("GridMap", m_black, D3DTADDRESS_WRAP, D3DTEXF_NONE, FALSE);
    bind("EnvironmentMap", m_env, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, TRUE);
    bind("ReflectionMap", reflection ? static_cast<IDirect3DBaseTexture9*>(reflection) : m_clear, D3DTADDRESS_CLAMP,
         D3DTEXF_NONE, TRUE);
    bind("RefractionMap", m_snapshotTaken && m_refr ? static_cast<IDirect3DBaseTexture9*>(m_refr) : m_black,
         D3DTADDRESS_CLAMP, D3DTEXF_NONE, TRUE);
    int shadowStage = psInfo.Reg("ShadowMap", 3);
    if (shadowStage >= 0)
    {
        m_dev->SetTexture(shadowStage, shadowTexture);
        Shadows::BindSampler(m_dev, shadowStage);
    }
    if (useShore)
    {
        // The glint turned from the sun now (towards it: the travel direction reversed, kept a little above the
        // horizon) onto the painted one: rotation R with R toSun = p, as rows (the shader turns a direction v by v x mtxGlint).
        float rot[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        // Wave shading's light: the glint's (the painted sun's when the glint stays put).
        float waveLight[4] = {kPaintedSun[0], kPaintedSun[1], kPaintedSun[2], g_config.waveShading / 255.0f};
        if (glint && g_config.glintFollowsSun == 1 && dayFraction >= 0)
        {
            // Logged as the sun's glint starts across and as the moon's takes over (and on the first draw).
            const float f = dayFraction - std::floor(dayFraction);
            const int night = f >= 0.25f && f < 0.75f ? 0 : 1;
            if (night != m_glintNight)
                Log::Write("water: the %s's glint, %.2f of the way round (right to left), brightness %.2f",
                           night ? "moon" : "sun", GlintAcross(), GlintFade());
            m_glintNight = night;
        }
        if (glint)
        {
            float toSun[4];
            GlintLight(vp, toSun);
            memcpy(waveLight, toSun, 3 * sizeof(float));
            GlintTurn(toSun, rot);
        }
        // The shader turns v by mul(v, mtxGlint) = v^T M with M = the registers' transpose (column-major): R v.
        int glintReg = psInfo.Reg("mtxGlint", 2);
        if (glintReg >= 0)
            m_dev->SetPixelShaderConstantF(glintReg, rot, 3);
        const float shoreConsts[4] = {shoreReady ? g_config.caustics : 0.0f, 1.0f - GlintFade(),
                                      fmodf(t, 3600.0f), glint ? 1.0f : 0.0f};
        int shoreReg = psInfo.Reg("f4Shore", 2);
        if (shoreReg >= 0)
            m_dev->SetPixelShaderConstantF(shoreReg, shoreConsts, 1);
        int waveLightReg = psInfo.Reg("f4WaveLight", 2);
        if (waveLightReg >= 0)
            m_dev->SetPixelShaderConstantF(waveLightReg, waveLight, 1);
        // Caustics: a 256-texel tile over 400 world units (two and a bit plots: its light cells about 20 units across,
        // big enough to see as a network at the game's zoom), the two layers drifting apart.
        auto wrap = [](float v) { return v - std::floor(v); };
        const float shoreGridConsts[4] = {shoreGrid[0], shoreGrid[1], 1.0f / 400.0f, 0};
        const float scroll[4] = {wrap(t * 0.011f), wrap(t * 0.006f), wrap(-t * 0.008f), wrap(t * 0.010f)};
        setVs("f4ShoreGrid", shoreGridConsts, 1);
        setVs("f4ShoreScroll", scroll, 1);
        bind("ShoreMap", shoreReady ? static_cast<IDirect3DBaseTexture9*>(shoreTexture) : m_clear, D3DTADDRESS_WRAP,
             D3DTEXF_NONE, FALSE);
        bind("CausticMap1", m_caustic[0] ? static_cast<IDirect3DBaseTexture9*>(m_caustic[0]) : m_clear,
             D3DTADDRESS_WRAP, D3DTEXF_LINEAR, FALSE);
        bind("CausticMap2", m_caustic[1] ? static_cast<IDirect3DBaseTexture9*>(m_caustic[1]) : m_clear,
             D3DTADDRESS_WRAP, D3DTEXF_LINEAR, FALSE);
        if (!m_loggedShore)
        {
            m_loggedShore = true;
            Log::Write("water: drawn with the shoreline water (glint follows the sun %s, shore %s, wave shading %d/255)",
                       glint ? "on" : "off", shoreReady ? "known" : "not read yet or off", g_config.waveShading);
        }
    }
    m_dev->SetVertexShader(useShore ? m_shoreVs : m_vs);
    m_dev->SetPixelShader(useShore ? m_shorePs : m_ps);

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

    float time = g_config.waterTime >= 0 ? g_config.waterTime : (GetTickCount() - m_startTick) / 1000.0f;
    float tex1[12], tex2[12];
    NormalLayers(time, tex1, tex2);
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const float kColonizationSun[3] = {0.999f, 0.920647f, 0.736518f};
    const float* sunRgb = sunColour ? sunColour : kColonizationSun;
    const float sun[4] = {sunRgb[0] * sunRgb[0], sunRgb[1] * sunRgb[1], sunRgb[2] * sunRgb[2], 0};
    const float waterShadow[4] = {0, shadowTexture ? 1.0f : 0.0f, shadowDarkness, 0};
    static const float kNoTint[3] = {1, 1, 1};
    const float* tint = skyLight ? skyLight : kNoTint;
    const float sky[4] = {tint[0], tint[1], tint[2], 0};

    // --- save BtS's state
    const int kSurfaceVs = 23, kSurfacePs = 5, kSurfaceStages = 4;
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
    const float normalFormat[4] = {CurrentNormalNm() ? 1.0f : 0.0f, 0, 0, 0};
    m_dev->SetPixelShaderConstantF(4, normalFormat, 1);
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
    bind(0, CurrentNormal(), D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, FALSE);
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
