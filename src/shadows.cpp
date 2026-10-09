#include "shadows.h"
#include "matrix.h"
#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "river_flood_ps.h"
#include "river_flood_vs.h"
#include "river_ps.h"
#include "river_vs.h"
#include "shadow_blur_ps.h"
#include "water_shadow_ps.h"
#include "water_shadow_vs.h"
#include "terrain_shadow_ps.h"
#include "terrain_shadow_vs.h"

namespace
{
const float kDepthRange = 4000.0f;  // sun camera depth half-range, world units
// BtS's River.fx River_Shader passes (floodplain P0, river P1), by shader hash: our copies assume their exact code,
// including a light-vector register that isn't in their constant tables (see river_shadow.hlsl).
const uint64_t kRiverFloodVs = 0x140246271c1a123bull, kRiverFloodPs = 0x579d9dd87d7f3783ull;
const uint64_t kRiverVs = 0x4cbc89303161e055ull, kRiverPs = 0x1c78680e17f6d324ull;
// Shadow texture outside the shadowed area: light factor 1 (colour), coverage 0 (alpha).
const D3DCOLOR kNoShadow = 0x00FFFFFF;

std::vector<DWORD> Tokens(const BYTE* bytes, size_t size)
{
    std::vector<DWORD> v(size / 4);
    memcpy(v.data(), bytes, size);
    return v;
}

void Normalize(float* v)
{
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
        for (int i = 0; i < 3; ++i)
            v[i] /= l;
}

void Cross(const float* a, const float* b, float* out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float Dot(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// 4x4 from three register rows (a world matrix) plus (0, 0, 0, 1).
void WorldFromRows(const float* rows, float* w)
{
    memcpy(w, rows, 12 * sizeof(float));
    w[12] = w[13] = w[14] = 0;
    w[15] = 1;
}

const D3DRENDERSTATETYPE kCasterStates[] = {
    D3DRS_ZENABLE,         D3DRS_ZWRITEENABLE,    D3DRS_CULLMODE,         D3DRS_ALPHABLENDENABLE,
    D3DRS_SRCBLEND,        D3DRS_DESTBLEND,       D3DRS_BLENDOP,          D3DRS_COLORWRITEENABLE,
    D3DRS_FOGENABLE,       D3DRS_CLIPPLANEENABLE, D3DRS_SRGBWRITEENABLE,  D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_STENCILENABLE,   D3DRS_SCISSORTESTENABLE};
}

Shadows::Shadows(IDirect3DDevice9* device) : m_dev(device)
{
    m_blobTextures.Load(g_config.blobShadowTexture, "[shadows] BlobShadowTexture");
    m_noShadowTextures.Load(g_config.noShadowTextures, "[shadows] NoShadowTextures");
}

bool Shadows::EnsureShaders()
{
    if (m_terrainVs && m_terrainPs && m_blurPs && m_riverFloodVs && m_riverFloodPs && m_riverVs && m_riverPs && m_waterVs &&
        m_waterPs)
        return true;
    ULONG before = DeviceRefs(m_dev);
    auto vs = [&](const BYTE* code, size_t size, IDirect3DVertexShader9** out) {
        return *out || SUCCEEDED(m_dev->CreateVertexShader(Tokens(code, size).data(), out));
    };
    auto ps = [&](const BYTE* code, size_t size, IDirect3DPixelShader9** out) {
        return *out || SUCCEEDED(m_dev->CreatePixelShader(Tokens(code, size).data(), out));
    };
    bool ok = vs(g_terrainShadowVS, sizeof(g_terrainShadowVS), &m_terrainVs) &&
              ps(g_terrainShadowPS, sizeof(g_terrainShadowPS), &m_terrainPs) &&
              ps(g_shadowBlurPS, sizeof(g_shadowBlurPS), &m_blurPs) &&
              vs(g_riverFloodVS, sizeof(g_riverFloodVS), &m_riverFloodVs) &&
              ps(g_riverFloodPS, sizeof(g_riverFloodPS), &m_riverFloodPs) && vs(g_riverVS, sizeof(g_riverVS), &m_riverVs) &&
              ps(g_riverPS, sizeof(g_riverPS), &m_riverPs) && vs(g_waterShadowVS, sizeof(g_waterShadowVS), &m_waterVs) &&
              ps(g_waterShadowPS, sizeof(g_waterShadowPS), &m_waterPs);
    m_internalRefs += DeviceRefs(m_dev) - before;
    if (!ok)
    {
        Log::Write("shadows: creating our shaders failed; shadows off");
        enabled = false;
    }
    return ok;
}

bool Shadows::EnsureResources()
{
    if (m_cast && m_lit)
        return true;
    ULONG before = DeviceRefs(m_dev);
    UINT size = g_config.shadowSize;
    bool ok = SUCCEEDED(m_dev->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                             &m_cast, nullptr)) &&
              SUCCEEDED(m_cast->GetSurfaceLevel(0, &m_castSurf)) &&
              SUCCEEDED(m_dev->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                             &m_lit, nullptr)) &&
              SUCCEEDED(m_lit->GetSurfaceLevel(0, &m_litSurf));
    m_internalRefs += DeviceRefs(m_dev) - before;
    if (!ok)
    {
        Log::Write("shadows: creating the %ux%u shadow targets failed; shadows off", size, size);
        enabled = false;
        return false;
    }
    // Start the "previous frame" texture empty (no shadow) so the first frame can use it.
    SavedTarget saved;
    saved.Save(m_dev);
    m_dev->SetRenderTarget(0, m_litSurf);
    m_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    saved.Restore(m_dev);
    Log::Write("shadows: created %ux%u coverage and shadow targets (max radius %g, darkness %g, sun %g,%g,%g)", size,
               size, g_config.shadowMaxRadius, g_config.shadowDarkness, g_config.sunDirection[0], g_config.sunDirection[1],
               g_config.sunDirection[2]);
    return true;
}

void Shadows::ReleaseDefault()
{
    m_frameSetup = false;
    m_havePrev = false;
    if (!m_cast && !m_castSurf && !m_lit && !m_litSurf && !m_blurState && !m_overlayState)
        return;  // nothing held (also runs from the destructor after the device is gone)
    ULONG before = DeviceRefs(m_dev);
    IUnknown* objects[] = {m_castSurf, m_cast, m_litSurf, m_lit, m_blurState, m_overlayState};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_castSurf = nullptr;
    m_cast = nullptr;
    m_litSurf = nullptr;
    m_lit = nullptr;
    m_blurState = nullptr;
    m_overlayState = nullptr;
    ULONG after = DeviceRefs(m_dev);
    m_internalRefs -= (before > after && m_internalRefs >= before - after) ? before - after : 0;
}

void Shadows::ReleaseAll()
{
    ReleaseDefault();
    IUnknown* objects[] = {m_terrainVs, m_terrainPs,   m_blurPs, m_riverFloodVs, m_riverFloodPs,
                           m_riverVs,   m_riverPs,     m_waterVs, m_waterPs};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_terrainVs = m_riverFloodVs = m_riverVs = m_waterVs = nullptr;
    m_terrainPs = m_blurPs = m_riverFloodPs = m_riverPs = m_waterPs = nullptr;
    m_internalRefs = 0;
    m_blobTextures.Clear();
    m_noShadowTextures.Clear();
    m_paintedTextures.clear();
}

bool Shadows::TargetIsBackBuffer()
{
    IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
    m_dev->GetRenderTarget(0, &rt);
    m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    bool same = rt && rt == bb;
    if (rt)
        rt->Release();
    if (bb)
        bb->Release();
    return same;
}

void Shadows::NewFrame()
{
    m_frameSetup = false;
    m_frameFailed = false;
    m_haveAnchor = false;
    m_fowTexture = nullptr;
    m_frameCasters = m_frameReceivers = 0;
}

bool Shadows::MainViewProj(const ShaderInfo* vs, bool fixedFunction, float* vp)
{
    if (vs && vs->viewProj >= 0)
        return SUCCEEDED(m_dev->GetVertexShaderConstantF(vs->viewProj, vp, 4));
    int wvpReg = -1, worldReg = -1;
    if (vs && vs->tree)
        wvpReg = vs->Reg("WorldViewProj", 2), worldReg = vs->Reg("World", 2);
    else if (vs && vs->btsTerrain)
        wvpReg = vs->Reg("mtxWorldViewProj", 2), worldReg = vs->world;
    if (wvpReg >= 0 && worldReg >= 0)
    {
        float wvp[16], rows[12], world[16], inv[16];
        if (FAILED(m_dev->GetVertexShaderConstantF(wvpReg, wvp, 4)) ||
            FAILED(m_dev->GetVertexShaderConstantF(worldReg, rows, 3)))
            return false;
        WorldFromRows(rows, world);
        if (!mat::Invert(world, inv))
            return false;
        mat::Mul(wvp, inv, vp);  // world -> clip
        return true;
    }
    if (!vs && fixedFunction)
    {
        // Row vectors: clip = p * VIEW * PROJECTION; register form is the transpose.
        D3DMATRIX view, proj;
        if (FAILED(m_dev->GetTransform(D3DTS_VIEW, &view)) || FAILED(m_dev->GetTransform(D3DTS_PROJECTION, &proj)))
            return false;
        float vpRow[16];  // VIEW * PROJECTION (row-vector form)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                vpRow[i * 4 + j] = view.m[i][0] * proj.m[0][j] + view.m[i][1] * proj.m[1][j] +
                                   view.m[i][2] * proj.m[2][j] + view.m[i][3] * proj.m[3][j];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                vp[i * 4 + j] = vpRow[j * 4 + i];
        return true;
    }
    return false;
}

namespace
{
// The ground point (z = h) the main camera shows at clip (cx, cy): solve clip x = cx w, clip y = cy w. False if the
// ray misses the ground (above the horizon) or the camera is degenerate.
bool GroundPoint(const float* vp, float h, float cx, float cy, float* out)
{
    float a0 = vp[0] - cx * vp[12], a1 = vp[1] - cx * vp[13], a2 = (vp[2] - cx * vp[14]) * h + vp[3] - cx * vp[15];
    float b0 = vp[4] - cy * vp[12], b1 = vp[5] - cy * vp[13], b2 = (vp[6] - cy * vp[14]) * h + vp[7] - cy * vp[15];
    float det = a0 * b1 - a1 * b0;
    if (std::fabs(det) < 1e-12f)
        return false;
    out[0] = (a1 * b2 - a2 * b1) / det;
    out[1] = (b0 * a2 - a0 * b2) / det;
    out[2] = h;
    return vp[12] * out[0] + vp[13] * out[1] + vp[14] * h + vp[15] > 0;  // in front of the camera
}
}

void Shadows::SetupFrame(const float* vp)
{
    if (m_frameFailed || !EnsureResources())
        return;
    const float h = g_config.waterHeight, maxR = g_config.shadowMaxRadius;
    // The view's footprint on the ground (z = water height): the screen centre and corners. A corner above the
    // horizon, or very far away, is pulled in along the screen edge towards the centre line.
    float c[3];
    if (!GroundPoint(vp, h, 0, 0, c))
    {
        m_frameFailed = true;  // e.g. looking at the sky: no shadows this frame
        return;
    }
    float pts[5][3] = {{c[0], c[1], c[2]}};
    int n = 1;
    for (float cx : {-1.0f, 1.0f})
        for (float cy : {-1.0f, 1.0f})
            for (float k = 1.0f; k > 0.05f; k -= 0.15f)
            {
                float p[3];
                if (GroundPoint(vp, h, cx, cy * k, p) && std::hypot(p[0] - c[0], p[1] - c[1]) < 3 * maxR)
                {
                    memcpy(pts[n++], p, sizeof(p));
                    break;
                }
            }

    // Orthographic sun camera looking along the sun's travel direction.
    float f[3] = {sunDirection[0], sunDirection[1], sunDirection[2]};
    Normalize(f);
    float up[3] = {0, 0, 1};
    if (std::fabs(f[2]) > 0.99f)
        up[1] = 1, up[2] = 0;
    float r[3], u[3];
    Cross(f, up, r);
    Normalize(r);
    Cross(r, f, u);

    // Fit the footprint in the sun's view, plus a margin for casters standing at the edges. Half-widths are rounded
    // up to 64 units and the centre is snapped to whole texels, so panning doesn't make the shadows shimmer.
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (int i = 0; i < n; ++i)
    {
        float q[2] = {Dot(r, pts[i]), Dot(u, pts[i])};
        for (int k = 0; k < 2; ++k)
            lo[k] = (std::min)(lo[k], q[k]), hi[k] = (std::max)(hi[k], q[k]);
    }
    float half[2], mid[2];
    for (int k = 0; k < 2; ++k)
    {
        half[k] = std::ceil(((hi[k] - lo[k]) * 0.5f * 1.05f + 60.0f) / 64.0f) * 64.0f;
        mid[k] = (lo[k] + hi[k]) * 0.5f;
        if (half[k] > maxR)
        {
            // Too big (zoomed far out): keep the part nearest the screen centre.
            half[k] = maxR;
            float centre = k == 0 ? Dot(r, c) : Dot(u, c);
            mid[k] = (std::max)(lo[k] + maxR, (std::min)(hi[k] - maxR, centre));
        }
        const float texel = 2.0f * half[k] / static_cast<float>(g_config.shadowSize);
        mid[k] = std::floor(mid[k] / texel) * texel;
    }
    float cf = Dot(f, c);
    float* m = m_lightVP;
    for (int i = 0; i < 3; ++i)
    {
        m[0 + i] = r[i] / half[0];
        m[4 + i] = u[i] / half[1];
        m[8 + i] = f[i] / (2.0f * kDepthRange);
        m[12 + i] = 0;
    }
    m[3] = -mid[0] / half[0];
    m[7] = -mid[1] / half[1];
    m[11] = 0.5f - cf / (2.0f * kDepthRange);
    m[15] = 1;
    mat::TextureProjection(m_lightVP, m_curShadowTex);

    SavedTarget saved;
    saved.Save(m_dev);
    m_dev->SetRenderTarget(0, m_castSurf);
    m_dev->SetDepthStencilSurface(nullptr);
    m_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    saved.Restore(m_dev);
    m_frameSetup = true;
    if (!m_loggedSetup)
    {
        m_loggedSetup = true;
        Log::Write("shadows: first sun camera centred at %g,%g,%g, area %gx%g", c[0], c[1], c[2], 2 * half[0],
                   2 * half[1]);
    }
}

bool Shadows::BlobShadow()
{
    if (!enabled)
        return false;
    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(m_dev->GetTexture(0, &base)) || !base)
        return false;
    const bool blob = m_blobTextures.Contains(base);
    base->Release();
    if (blob)
    {
        // BtS draws each unit's blob just before the unit's own meshes: its position anchors the unit's rigid parts.
        D3DMATRIX world;
        if (SUCCEEDED(m_dev->GetTransform(D3DTS_WORLD, &world)))
        {
            m_anchor[0] = world._41;
            m_anchor[1] = world._42;
            m_anchor[2] = world._43;
            m_haveAnchor = true;
        }
    }
    return blob;
}

namespace
{
// Black texture: every colour in the top level at most 64 of 255 (BtS's painted shadows: <= 8, the courthouse's 49;
// roads, resources and the oil seep: 246-255). DXT: the two colour endpoints of every block; A8R8G8B8: every texel.
bool BlackTexture(IDirect3DTexture9* tex)
{
    D3DSURFACE_DESC d = {};
    if (FAILED(tex->GetLevelDesc(0, &d)) || d.Pool == D3DPOOL_DEFAULT)
        return false;
    const bool dxt1 = d.Format == D3DFMT_DXT1, dxt35 = d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5,
               argb = d.Format == D3DFMT_A8R8G8B8;
    if (!dxt1 && !dxt35 && !argb)
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
        return false;
    const int kMax = 64;
    bool black = true;
    if (argb)
    {
        for (UINT y = 0; y < d.Height && black; ++y)
        {
            const DWORD* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch);
            for (UINT x = 0; x < d.Width && black; ++x)
                black = ((row[x] >> 16) & 0xFF) <= kMax && ((row[x] >> 8) & 0xFF) <= kMax && (row[x] & 0xFF) <= kMax;
        }
    }
    else
    {
        const UINT blockBytes = dxt1 ? 8 : 16, colourAt = dxt1 ? 0 : 8;
        const UINT bw = (std::max)(1u, d.Width / 4), bh = (std::max)(1u, d.Height / 4);
        for (UINT by = 0; by < bh && black; ++by)
        {
            const BYTE* row = static_cast<const BYTE*>(lr.pBits) + by * lr.Pitch;
            for (UINT bx = 0; bx < bw && black; ++bx)
                for (int k = 0; k < 2 && black; ++k)
                {
                    WORD c = *reinterpret_cast<const WORD*>(row + bx * blockBytes + colourAt + k * 2);  // RGB 565
                    int r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
                    black = r <= kMax && g <= kMax && b <= kMax;
                }
        }
    }
    tex->UnlockRect(0);
    return black;
}
}

bool Shadows::SamplesFogOfWar(DWORD stages)
{
    // World decals sample this frame's fog-of-war texture (the terrain draw's s1); interface overlays drawn on the
    // ground (area borders, World Builder highlights, Dark_Background_Overlay.dds) don't.
    for (DWORD s = 0; s < stages && m_fowTexture; ++s)
    {
        IDirect3DBaseTexture9* t = nullptr;
        m_dev->GetTexture(s, &t);
        bool fow = t == m_fowTexture;
        if (t)
            t->Release();
        if (fow)
            return true;
    }
    return false;
}

bool Shadows::PaintedShadow()
{
    DWORD blend = FALSE;
    m_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    IDirect3DBaseTexture9* base = nullptr;
    if (!blend || !SamplesFogOfWar(8) || FAILED(m_dev->GetTexture(0, &base)) || !base)
        return false;
    bool painted = false;
    if (base->GetType() == D3DRTYPE_TEXTURE)
    {
        // Only "black" is cached (and dropped when a new texture takes the address: ForgetTexture). A "no" is checked
        // again each time: cheap, as the scan stops at the first coloured block, and safe if the game reuses an
        // address or fills a texture after first drawing it.
        painted = m_paintedTextures.count(base) != 0;
        if (!painted && BlackTexture(static_cast<IDirect3DTexture9*>(base)))
        {
            m_paintedTextures.emplace(base, true);
            painted = true;
        }
    }
    base->Release();
    return painted;
}

bool Shadows::BuildingPart(DWORD fvf)
{
    // Buildings, improvements, wonders: solid (depth-writing) lit fixed-function meshes. Ground decals and interface
    // overlays don't write depth; units' skinned and rigid parts are found separately.
    if (!g_config.buildingShadows || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || !(fvf & D3DFVF_NORMAL))
        return false;
    DWORD zwrite = FALSE, lighting = FALSE;
    m_dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    m_dev->GetRenderState(D3DRS_LIGHTING, &lighting);
    // Not things floating on the water (resource wave rings: clams, fish), only what stands above it.
    D3DMATRIX world;
    if (!zwrite || !lighting || FAILED(m_dev->GetTransform(D3DTS_WORLD, &world)) ||
        world._43 < g_config.waterHeight + 5.0f)
        return false;
    return !PaintedShadow();
}

bool Shadows::NoShadowTexture()
{
    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(m_dev->GetTexture(0, &base)) || !base)
        return false;
    const bool listed = m_noShadowTextures.Contains(base);
    base->Release();
    return listed;
}

bool Shadows::RigidUnitPart(DWORD fvf)
{
    // Weapons, shields, catapult parts...: fixed-function, not skinned, lit (normals), writing depth, and placed
    // where the last blob shadow was (within a third of a plot, from the ground to well above head height).
    if (!m_haveAnchor || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || !(fvf & D3DFVF_NORMAL))
        return false;
    DWORD zwrite = FALSE;
    m_dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    D3DMATRIX world;
    if (!zwrite || FAILED(m_dev->GetTransform(D3DTS_WORLD, &world)))
        return false;
    float dz = world._43 - m_anchor[2];
    return std::hypot(world._41 - m_anchor[0], world._42 - m_anchor[1]) < 60.0f && dz > -30.0f && dz < 150.0f;
}

bool Shadows::MaybeReceiver(const ShaderInfo& vs)
{
    return vs.btsTerrain || vs.hash == kRiverFloodVs || vs.hash == kRiverVs;
}

bool Shadows::DrawReceiver(const ShaderInfo& vs, const ShaderInfo* ps, const std::function<HRESULT()>& draw,
                           HRESULT& hr)
{
    if ((!enabled && !terrainLight) || !ps)
        return false;
    // BtS's terrain shader with exactly the registers and samplers our copy assumes, or one of the two river passes.
    IDirect3DVertexShader9** ourVs = nullptr;
    IDirect3DPixelShader9** ourPs = nullptr;
    bool terrain = false;
    if (vs.btsTerrain && vs.Reg("mtxWorldViewProj", 2) == 0 && vs.world == 4 && vs.Reg("mtxFOW", 2) == 7 &&
        vs.Reg("mtxLightmap", 2) == 9 && vs.Reg("fDetailTexScaling", 2) == 11 && ps->Reg("TerrainBase", 3) == 0 &&
        ps->Reg("TerrainFOWar", 3) == 1 && ps->Reg("TerrainLightmap", 3) == 2 && ps->Reg("TerrainDetail", 3) == 3)
        ourVs = &m_terrainVs, ourPs = &m_terrainPs, terrain = true;
    else if (!enabled)
        return false;  // lighting alone: only the terrain needs our shader (the rivers' colours are constants)
    else if (vs.hash == kRiverFloodVs && ps->hash == kRiverFloodPs)
        ourVs = &m_riverFloodVs, ourPs = &m_riverFloodPs;
    else if (vs.hash == kRiverVs && ps->hash == kRiverPs)
        ourVs = &m_riverVs, ourPs = &m_riverPs;
    else
        return false;
    if (!TargetIsBackBuffer())
        return false;
    if (enabled && terrain && !m_frameSetup)
    {
        float vp[16];
        if (MainViewProj(&vs, false, vp))
            SetupFrame(vp);
    }
    if ((enabled && !m_havePrev && !terrainLight) || !EnsureShaders())
        return false;
    if (terrain)
    {
        // The fog-of-war texture (TerrainFOWar, s1): world decals sample it too, interface overlays don't (DrawDecal).
        IDirect3DBaseTexture9* fow = nullptr;
        m_dev->GetTexture(1, &fow);
        m_fowTexture = fow;  // compared only, never used
        if (fow)
            fow->Release();
    }
    hr = DrawShadowed(*ourVs, *ourPs, draw, terrain);
    ++m_frameReceivers;
    return true;
}

namespace
{
const D3DSAMPLERSTATETYPE kShadowSamplerStates[8] = {
    D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_ADDRESSW,    D3DSAMP_BORDERCOLOR,
    D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE};

// Shadow texture sampler: bilinear, no mips, and outside the area BORDER = no shadow.
void SetShadowSampler(IDirect3DDevice9* dev, DWORD stage)
{
    const DWORD values[8] = {D3DTADDRESS_BORDER, D3DTADDRESS_BORDER, D3DTADDRESS_BORDER, kNoShadow,
                             D3DTEXF_LINEAR,     D3DTEXF_LINEAR,     D3DTEXF_NONE,       FALSE};
    for (int k = 0; k < 8; ++k)
        dev->SetSamplerState(stage, kShadowSamplerStates[k], values[k]);
}
}

void Shadows::BindSampler(IDirect3DDevice9* dev, DWORD stage)
{
    SetShadowSampler(dev, stage);
}

HRESULT Shadows::DrawShadowed(IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps,
                              const std::function<HRESULT()>& draw, bool terrain)
{
    // Our copies use the game's own constants and textures; they add VS c20-c23 (world -> shadow texture), PS c10
    // (darkness) and s4 (the shadow texture).
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DBaseTexture9* oldTex = nullptr;
    float oldVsc[16], oldPsc[4], oldLight[32], oldHillGrid[4];
    IDirect3DBaseTexture9 *oldNormals = nullptr, *oldHills = nullptr;
    DWORD oldSamp5[8], oldSamp6[8];
    DWORD oldSamp[8];
    m_dev->GetVertexShader(&oldVs);
    m_dev->GetPixelShader(&oldPs);
    m_dev->GetVertexShaderConstantF(20, oldVsc, 4);
    m_dev->GetPixelShaderConstantF(10, oldPsc, 1);
    if (terrain)
    {
        m_dev->GetPixelShaderConstantF(11, oldLight, 8);
        m_dev->GetTexture(5, &oldNormals);
        for (int k = 0; k < 8; ++k)
            m_dev->GetSamplerState(5, kShadowSamplerStates[k], &oldSamp5[k]);
    }
    else
        m_dev->GetPixelShaderConstantF(11, oldHillGrid, 1);
    m_dev->GetTexture(6, &oldHills);
    for (int k = 0; k < 8; ++k)
        m_dev->GetSamplerState(6, kShadowSamplerStates[k], &oldSamp6[k]);
    m_dev->GetTexture(4, &oldTex);
    for (int k = 0; k < 8; ++k)
        m_dev->GetSamplerState(4, kShadowSamplerStates[k], &oldSamp[k]);

    m_dev->SetVertexShader(vs);
    m_dev->SetPixelShader(ps);
    m_dev->SetVertexShaderConstantF(20, m_prevShadowTex, 4);
    // Shadows off (terrain drawn here only for the lighting): darkness 0, so the shadow texture doesn't matter.
    const bool shadowed = enabled && m_havePrev;
    const float shadow[4] = {shadowed ? g_config.shadowDarkness * darknessScale : 0.0f, 0, shadowed ? 1.0f : 0.0f, 0};
    m_dev->SetPixelShaderConstantF(10, shadow, 1);
    if (terrain)
    {
        float light[32] = {};
        if (terrainLight)
            memcpy(light, terrainLight, sizeof(light));
        if (!terrainNormals)
            light[30] = 0;  // no normal map (yet): no per-pixel change
        if (terrainGrid[0] > 0)
            light[28] = terrainGrid[0], light[29] = terrainGrid[1];
        light[31] = shadowed && hillShadows ? 1.0f : 0.0f;
        m_dev->SetPixelShaderConstantF(11, light, 8);  // c15.x = 0: lightmap as baked
        // Normal map: bilinear, wrapping like the map; no mips.
        m_dev->SetTexture(5, terrainNormals);
        const DWORD values[8] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, 0,
                                 D3DTEXF_LINEAR,   D3DTEXF_LINEAR,   D3DTEXF_NONE,     FALSE};
        for (int k = 0; k < 8; ++k)
            m_dev->SetSamplerState(5, kShadowSamplerStates[k], values[k]);
    }
    else
    {
        const float grid[4] = {terrainGrid[0], terrainGrid[1], 0, shadowed && hillShadows ? 1.0f : 0.0f};
        m_dev->SetPixelShaderConstantF(11, grid, 1);
    }
    {
        // Hill shadow mask: bilinear, wrapping like the map; no mips.
        m_dev->SetTexture(6, shadowed ? hillShadows : nullptr);
        const DWORD values[8] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, 0,
                                 D3DTEXF_LINEAR,   D3DTEXF_LINEAR,   D3DTEXF_NONE,     FALSE};
        for (int k = 0; k < 8; ++k)
            m_dev->SetSamplerState(6, kShadowSamplerStates[k], values[k]);
    }
    m_dev->SetTexture(4, shadowed ? m_lit : nullptr);
    SetShadowSampler(m_dev, 4);

    HRESULT hr = draw();

    m_dev->SetVertexShader(oldVs);
    m_dev->SetPixelShader(oldPs);
    m_dev->SetVertexShaderConstantF(20, oldVsc, 4);
    m_dev->SetPixelShaderConstantF(10, oldPsc, 1);
    if (terrain)
    {
        m_dev->SetPixelShaderConstantF(11, oldLight, 8);
        m_dev->SetTexture(5, oldNormals);
        for (int k = 0; k < 8; ++k)
            m_dev->SetSamplerState(5, kShadowSamplerStates[k], oldSamp5[k]);
        if (oldNormals)
            oldNormals->Release();
    }
    else
        m_dev->SetPixelShaderConstantF(11, oldHillGrid, 1);
    m_dev->SetTexture(6, oldHills);
    for (int k = 0; k < 8; ++k)
        m_dev->SetSamplerState(6, kShadowSamplerStates[k], oldSamp6[k]);
    if (oldHills)
        oldHills->Release();
    m_dev->SetTexture(4, oldTex);
    for (int k = 0; k < 8; ++k)
        m_dev->SetSamplerState(4, kShadowSamplerStates[k], oldSamp[k]);
    for (IUnknown* o : {static_cast<IUnknown*>(oldVs), static_cast<IUnknown*>(oldPs), static_cast<IUnknown*>(oldTex)})
        if (o)
            o->Release();
    return hr;
}

bool Shadows::ShadeWater(const std::function<HRESULT()>& draw)
{
    if (!enabled || !m_havePrev || g_config.waterShadow <= 0 || !TargetIsBackBuffer() || !EnsureShaders())
        return false;
    // Save what we change.
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    m_dev->GetVertexShader(&oldVs);
    m_dev->GetPixelShader(&oldPs);
    float oldVsc[16], oldPsc[4];
    m_dev->GetVertexShaderConstantF(20, oldVsc, 4);
    m_dev->GetPixelShaderConstantF(0, oldPsc, 1);
    IDirect3DBaseTexture9* oldTex = nullptr;
    m_dev->GetTexture(0, &oldTex);
    DWORD oldSamp[8];
    for (int k = 0; k < 8; ++k)
        m_dev->GetSamplerState(0, kShadowSamplerStates[k], &oldSamp[k]);
    const D3DRENDERSTATETYPE states[] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,         D3DRS_DESTBLEND,
                                         D3DRS_BLENDOP,          D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC,
                                         D3DRS_ALPHATESTENABLE,  D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE,
                                         D3DRS_SRGBWRITEENABLE,  D3DRS_SEPARATEALPHABLENDENABLE};
    DWORD oldStates[sizeof(states) / sizeof(states[0])];
    for (size_t k = 0; k < sizeof(states) / sizeof(states[0]); ++k)
        m_dev->GetRenderState(states[k], &oldStates[k]);

    // Screen x light factor: the water as drawn, and what shows through it, darkened where the shadow lies.
    m_dev->SetVertexShader(m_waterVs);
    m_dev->SetPixelShader(m_waterPs);
    m_dev->SetVertexShaderConstantF(20, m_prevShadowTex, 4);
    const float strength[4] = {(std::min)(1.0f, g_config.waterShadow), 0, 0, 0};
    m_dev->SetPixelShaderConstantF(0, strength, 1);
    m_dev->SetTexture(0, m_lit);
    SetShadowSampler(m_dev, 0);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    m_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);  // the same surface again
    m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                      D3DCOLORWRITEENABLE_BLUE);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    draw();

    for (size_t k = 0; k < sizeof(states) / sizeof(states[0]); ++k)
        m_dev->SetRenderState(states[k], oldStates[k]);
    m_dev->SetTexture(0, oldTex);
    if (oldTex)
        oldTex->Release();
    for (int k = 0; k < 8; ++k)
        m_dev->SetSamplerState(0, kShadowSamplerStates[k], oldSamp[k]);
    m_dev->SetVertexShaderConstantF(20, oldVsc, 4);
    m_dev->SetPixelShaderConstantF(0, oldPsc, 1);
    m_dev->SetVertexShader(oldVs);
    m_dev->SetPixelShader(oldPs);
    if (oldVs)
        oldVs->Release();
    if (oldPs)
        oldPs->Release();
    return true;
}

bool Shadows::DrawDecal(const std::function<HRESULT()>& draw, HRESULT& hr)
{
    // Fixed-function ground decals (roads, river foam): blended, no depth writes, no skinning, drawn before the
    // frame's first unit (BtS draws its ground decals right after the terrain). Units' own parts are never darkened
    // this way, since their own coverage would shadow them.
    if (!enabled || !m_havePrev || m_haveAnchor || !m_lit)
        return false;
    IDirect3DPixelShader9* ps = nullptr;
    m_dev->GetPixelShader(&ps);
    if (ps)
    {
        ps->Release();
        return false;
    }
    DWORD vblend = 0, zwrite = 0, ablend = 0, src = 0, dst = 0, fvf = 0;
    m_dev->GetRenderState(D3DRS_VERTEXBLEND, &vblend);
    m_dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    m_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ablend);
    m_dev->GetRenderState(D3DRS_SRCBLEND, &src);
    m_dev->GetRenderState(D3DRS_DESTBLEND, &dst);
    m_dev->GetFVF(&fvf);
    // Alpha-blended (roads) or additive (river foam: ONE/ONE); either way the sunlit colour scales with the light.
    bool blendOk = (src == D3DBLEND_SRCALPHA || src == D3DBLEND_ONE) &&
                   (dst == D3DBLEND_INVSRCALPHA || dst == D3DBLEND_ONE);
    if (vblend != D3DVBF_DISABLE || zwrite || !ablend || !blendOk || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ ||
        !TargetIsBackBuffer())
        return false;

    // The first unused texture stage multiplies by the shadow texture's light factor (its colour channels). Its
    // coordinates are generated from the camera-space position: texture matrix = VIEW^-1 x (world -> shadow uv).
    if (!m_maxStages)
    {
        D3DCAPS9 caps = {};
        m_dev->GetDeviceCaps(&caps);
        m_maxStages = (std::min)((std::min)(caps.MaxTextureBlendStages, caps.MaxSimultaneousTextures), DWORD(8));
    }
    DWORD stage = 0;
    for (DWORD op = 0; stage < m_maxStages; ++stage)
        if (SUCCEEDED(m_dev->GetTextureStageState(stage, D3DTSS_COLOROP, &op)) && op == D3DTOP_DISABLE)
            break;
    D3DMATRIX view;
    float invView[16];
    if (stage >= m_maxStages || FAILED(m_dev->GetTransform(D3DTS_VIEW, &view)) || !mat::Invert(&view._11, invView))
        return false;
    // Part of the world (roads on stage 1, river foam on stage 2); interface overlays stay as they are.
    if (!SamplesFogOfWar(stage))
        return false;
    float shadowRows[16], texMat[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            shadowRows[i * 4 + j] = m_prevShadowTex[j * 4 + i];  // row-vector form
    mat::Mul(invView, shadowRows, texMat);
    // Hill shadows too, on the next stage if there's one: same coordinates generation, matrix VIEW^-1 x (world xy ->
    // grid uv), the mask's colour = its light factor.
    const bool hill = hillShadows && terrainGrid[0] > 0 && stage + 1 < m_maxStages;
    float hillMat[16] = {};
    if (hill)
    {
        float grid[16] = {};
        grid[0] = grid[5] = terrainGrid[0];  // u = x * scale + offset, v = y * scale + offset (row-vector form)
        grid[12] = grid[13] = terrainGrid[1];
        grid[15] = 1;
        mat::Mul(invView, grid, hillMat);
    }
    const DWORD used = hill ? 2 : 1;

    // Save what the stages we use (and the one after, which we disable) had.
    const D3DTEXTURESTAGESTATETYPE states[9] = {D3DTSS_COLOROP,   D3DTSS_COLORARG1,     D3DTSS_COLORARG2,
                                                D3DTSS_ALPHAOP,   D3DTSS_ALPHAARG1,     D3DTSS_TEXCOORDINDEX,
                                                D3DTSS_RESULTARG, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_ALPHAARG2};
    struct SavedStage
    {
        DWORD state[9], sampler[8];
        D3DMATRIX matrix;
        IDirect3DBaseTexture9* texture;
    } saved[2] = {};
    DWORD oldNextColor = D3DTOP_DISABLE, oldNextAlpha = D3DTOP_DISABLE;
    const bool next = stage + used < 8;
    for (DWORD u = 0; u < used; ++u)
    {
        SavedStage& sv = saved[u];
        for (int k = 0; k < 9; ++k)
            m_dev->GetTextureStageState(stage + u, states[k], &sv.state[k]);
        for (int k = 0; k < 8; ++k)
            m_dev->GetSamplerState(stage + u, kShadowSamplerStates[k], &sv.sampler[k]);
        m_dev->GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage + u), &sv.matrix);
        m_dev->GetTexture(stage + u, &sv.texture);
    }
    if (next)
    {
        m_dev->GetTextureStageState(stage + used, D3DTSS_COLOROP, &oldNextColor);
        m_dev->GetTextureStageState(stage + used, D3DTSS_ALPHAOP, &oldNextAlpha);
    }

    auto multiply = [&](DWORD st, IDirect3DBaseTexture9* tex, const float* matrix) {
        m_dev->SetTexture(st, tex);
        m_dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + st),
                            reinterpret_cast<const D3DMATRIX*>(matrix));
        m_dev->SetTextureStageState(st, D3DTSS_COLOROP, D3DTOP_MODULATE);
        m_dev->SetTextureStageState(st, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        m_dev->SetTextureStageState(st, D3DTSS_COLORARG2, D3DTA_CURRENT);
        m_dev->SetTextureStageState(st, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        m_dev->SetTextureStageState(st, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
        m_dev->SetTextureStageState(st, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        m_dev->SetTextureStageState(st, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
        m_dev->SetTextureStageState(st, D3DTSS_RESULTARG, D3DTA_CURRENT);
        m_dev->SetTextureStageState(st, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    };
    multiply(stage, m_lit, texMat);
    SetShadowSampler(m_dev, stage);
    if (hill)
    {
        multiply(stage + 1, hillShadows, hillMat);
        const DWORD values[8] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, 0,
                                 D3DTEXF_LINEAR,   D3DTEXF_LINEAR,   D3DTEXF_NONE,     FALSE};
        for (int k = 0; k < 8; ++k)
            m_dev->SetSamplerState(stage + 1, kShadowSamplerStates[k], values[k]);
    }
    if (next)
    {
        m_dev->SetTextureStageState(stage + used, D3DTSS_COLOROP, D3DTOP_DISABLE);
        m_dev->SetTextureStageState(stage + used, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    hr = draw();

    for (DWORD u = 0; u < used; ++u)
    {
        SavedStage& sv = saved[u];
        for (int k = 0; k < 9; ++k)
            m_dev->SetTextureStageState(stage + u, states[k], sv.state[k]);
        for (int k = 0; k < 8; ++k)
            m_dev->SetSamplerState(stage + u, kShadowSamplerStates[k], sv.sampler[k]);
        m_dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage + u), &sv.matrix);
        m_dev->SetTexture(stage + u, sv.texture);
        if (sv.texture)
            sv.texture->Release();
    }
    if (next)
    {
        m_dev->SetTextureStageState(stage + used, D3DTSS_COLOROP, oldNextColor);
        m_dev->SetTextureStageState(stage + used, D3DTSS_ALPHAOP, oldNextAlpha);
    }
    ++m_frameReceivers;
    return true;
}

bool Shadows::AfterDraw(const ShaderInfo* vs, bool fixedFunction, const std::function<void()>& redraw)
{
    if (!enabled)
        return false;
    bool unit = vs && vs->viewProj >= 0 && vs->worldBones >= 0;
    bool tree = vs && vs->tree;
    bool ffSkinned = false, rigid = false, building = false;
    if (!vs && fixedFunction)
    {
        DWORD blend = D3DVBF_DISABLE, fvf = 0;
        m_dev->GetRenderState(D3DRS_VERTEXBLEND, &blend);
        m_dev->GetFVF(&fvf);
        bool unitPart = blend == D3DVBF_DISABLE && RigidUnitPart(fvf);
        building = blend == D3DVBF_DISABLE && !unitPart && BuildingPart(fvf);
        rigid = unitPart || building;
        ffSkinned = blend != D3DVBF_DISABLE ? SkinnedLitFvf(fvf) : rigid;
    }
    if (!unit && !tree && !ffSkinned)
        return false;
    if (NoShadowTexture())
        return false;
    if (!TargetIsBackBuffer())
        return false;  // e.g. the HUD unit portrait
    if (!m_frameSetup)
    {
        float vp[16];
        if (!MainViewProj(vs, ffSkinned, vp))
            return false;
        SetupFrame(vp);
        if (!m_frameSetup)
            return false;
    }

    SavedTarget saved;
    saved.Save(m_dev);
    DWORD oldStates[sizeof(kCasterStates) / sizeof(kCasterStates[0])];
    for (size_t k = 0; k < sizeof(kCasterStates) / sizeof(kCasterStates[0]); ++k)
        m_dev->GetRenderState(kCasterStates[k], &oldStates[k]);

    m_dev->SetRenderTarget(0, m_castSurf);
    m_dev->SetDepthStencilSurface(nullptr);
    m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    m_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MAX);  // coverage = the most opaque caster at each texel
    m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

    if (unit)
    {
        // World-space bones, then mtxViewProj: swap in the sun camera.
        UINT reg = static_cast<UINT>(vs->viewProj);
        float old[16];
        m_dev->GetVertexShaderConstantF(reg, old, 4);
        m_dev->SetVertexShaderConstantF(reg, m_lightVP, 4);
        redraw();
        m_dev->SetVertexShaderConstantF(reg, old, 4);
    }
    else if (tree)
    {
        // Trees: WorldViewProj = sun camera * World.
        UINT wvpReg = static_cast<UINT>(vs->Reg("WorldViewProj", 2)), worldReg = static_cast<UINT>(vs->Reg("World", 2));
        float old[16], rows[12], world[16], wvp[16];
        m_dev->GetVertexShaderConstantF(wvpReg, old, 4);
        m_dev->GetVertexShaderConstantF(worldReg, rows, 3);
        WorldFromRows(rows, world);
        mat::Mul(m_lightVP, world, wvp);
        m_dev->SetVertexShaderConstantF(wvpReg, wvp, 4);
        redraw();
        m_dev->SetVertexShaderConstantF(wvpReg, old, 4);
    }
    else
    {
        // Fixed-function skinning: VIEW = identity, PROJECTION = the sun camera (row-vector form = transpose).
        // Buildings and units' rigid parts: only what's above the ground. BtS sinks building models partly into the
        // ground (no gaps on slopes); hidden by the terrain on screen, but the coverage texture has no depth, so
        // those foundations would shade the ground on the building's sunward side. A building's base is its origin;
        // a weapon's origin is the hand holding it (the club hangs below it), so its ground is its unit's blob.
        float oldPlane[4];
        m_dev->GetClipPlane(0, oldPlane);
        D3DMATRIX world;
        if (rigid && SUCCEEDED(m_dev->GetTransform(D3DTS_WORLD, &world)))
        {
            const float ground = building ? world._43 : m_anchor[2];
            const float plane[4] = {0, 0, 1, -(ground - 2.0f)};  // world z >= ground - 2 (fixed function: world space)
            m_dev->SetClipPlane(0, plane);
            m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
        }
        // Buildings' coverage goes into red (their own darkness, BuildingDarkness): one more texture stage copies the
        // final alpha into the colour, written to red only. Without a free stage they fall back to alpha.
        DWORD extra = 8, oldExtra[6] = {}, oldAfter[2] = {};
        IDirect3DBaseTexture9* oldExtraTex = nullptr;
        const D3DTEXTURESTAGESTATETYPE extraStates[6] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP,
                                                         D3DTSS_ALPHAARG1, D3DTSS_RESULTARG, D3DTSS_COLORARG2};
        if (building)
        {
            if (!m_maxStages)
            {
                D3DCAPS9 caps = {};
                m_dev->GetDeviceCaps(&caps);
                m_maxStages = (std::min)((std::min)(caps.MaxTextureBlendStages, caps.MaxSimultaneousTextures), DWORD(8));
            }
            for (DWORD st = 0, op = 0; st < m_maxStages; ++st)
                if (SUCCEEDED(m_dev->GetTextureStageState(st, D3DTSS_COLOROP, &op)) && op == D3DTOP_DISABLE)
                {
                    extra = st;
                    break;
                }
            if (extra < m_maxStages)
            {
                for (int k = 0; k < 6; ++k)
                    m_dev->GetTextureStageState(extra, extraStates[k], &oldExtra[k]);
                m_dev->GetTexture(extra, &oldExtraTex);
                if (extra + 1 < 8)
                {
                    m_dev->GetTextureStageState(extra + 1, D3DTSS_COLOROP, &oldAfter[0]);
                    m_dev->GetTextureStageState(extra + 1, D3DTSS_ALPHAOP, &oldAfter[1]);
                    m_dev->SetTextureStageState(extra + 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                    m_dev->SetTextureStageState(extra + 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                }
                m_dev->SetTexture(extra, nullptr);
                m_dev->SetTextureStageState(extra, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                m_dev->SetTextureStageState(extra, D3DTSS_COLORARG1, D3DTA_CURRENT | D3DTA_ALPHAREPLICATE);
                m_dev->SetTextureStageState(extra, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                m_dev->SetTextureStageState(extra, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
                m_dev->SetTextureStageState(extra, D3DTSS_RESULTARG, D3DTA_CURRENT);
                m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
            }
        }
        D3DMATRIX oldView, oldProj, view, proj;
        m_dev->GetTransform(D3DTS_VIEW, &oldView);
        m_dev->GetTransform(D3DTS_PROJECTION, &oldProj);
        memset(&view, 0, sizeof(view));
        view._11 = view._22 = view._33 = view._44 = 1;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                proj.m[i][j] = m_lightVP[j * 4 + i];
        m_dev->SetTransform(D3DTS_VIEW, &view);
        m_dev->SetTransform(D3DTS_PROJECTION, &proj);
        redraw();
        m_dev->SetTransform(D3DTS_VIEW, &oldView);
        m_dev->SetTransform(D3DTS_PROJECTION, &oldProj);
        m_dev->SetClipPlane(0, oldPlane);  // CLIPPLANEENABLE comes back with the other caster states
        if (building && extra < m_maxStages)
        {
            for (int k = 0; k < 6; ++k)
                m_dev->SetTextureStageState(extra, extraStates[k], oldExtra[k]);
            m_dev->SetTexture(extra, oldExtraTex);
            if (oldExtraTex)
                oldExtraTex->Release();
            if (extra + 1 < 8)
            {
                m_dev->SetTextureStageState(extra + 1, D3DTSS_COLOROP, oldAfter[0]);
                m_dev->SetTextureStageState(extra + 1, D3DTSS_ALPHAOP, oldAfter[1]);
            }
        }
    }

    for (size_t k = 0; k < sizeof(kCasterStates) / sizeof(kCasterStates[0]); ++k)
        m_dev->SetRenderState(kCasterStates[k], oldStates[k]);
    saved.Restore(m_dev);
    ++m_frameCasters;
    return true;
}

void Shadows::EndFrame()
{
    // No new shadows this frame (shadows off, or nothing on the ground: a full-screen menu or screen): the last ones
    // are out of date, so the next frame draws none rather than those, fixed where they were.
    m_havePrev = false;
    if (!enabled || !m_frameSetup || !EnsureShaders())
        return;
    if (!m_blurState)
    {
        ULONG before = DeviceRefs(m_dev);
        if (FAILED(m_dev->CreateStateBlock(D3DSBT_ALL, &m_blurState)))
            return;
        m_internalRefs += DeviceRefs(m_dev) - before;
    }
    SavedTarget saved;
    saved.Save(m_dev);
    m_blurState->Capture();
    m_dev->SetRenderTarget(0, m_litSurf);
    m_dev->SetDepthStencilSurface(nullptr);
    if (SUCCEEDED(m_dev->BeginScene()))
    {
        const float size = static_cast<float>(g_config.shadowSize);
        m_dev->SetVertexShader(nullptr);
        m_dev->SetPixelShader(m_blurPs);
        m_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        m_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        m_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        m_dev->SetTexture(0, m_cast);
        m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        m_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        m_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        m_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        // taps at +-Softness texels; fade to no shadow over the outer 3% (the area covers the view, so rarely seen)
        const float texel[4] = {g_config.shadowSoftness / size, g_config.shadowSoftness / size, 0.03f,
                                g_config.shadowDarkness * darknessScale};
        const float building[4] = {g_config.buildingDarkness * darknessScale, 0, 0, 0};
        m_dev->SetPixelShaderConstantF(1, building, 1);
        m_dev->SetPixelShaderConstantF(0, texel, 1);
        struct V
        {
            float x, y, z, rhw, u, v;
        } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0},
                  {size - 0.5f, -0.5f, 0, 1, 1, 0},
                  {-0.5f, size - 0.5f, 0, 1, 0, 1},
                  {size - 0.5f, size - 0.5f, 0, 1, 1, 1}};
        m_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        m_dev->EndScene();
        memcpy(m_prevShadowTex, m_curShadowTex, sizeof(m_prevShadowTex));
        m_havePrev = true;
    }
    m_blurState->Apply();
    saved.Restore(m_dev);
}

bool Shadows::SaveBmps(const std::wstring& dir)
{
    return m_castSurf && m_litSurf && SaveSurfaceBmp(m_dev, m_castSurf, dir + L"\\shadow_coverage.bmp") &&
           SaveSurfaceBmp(m_dev, m_litSurf, dir + L"\\shadow_blurred.bmp");
}

void Shadows::DrawOverlay()
{
    if (!overlay || !m_cast || !m_lit)
        return;
    // left: this frame's coverage; right: the blurred shadow texture the next frame's terrain uses
    const OverlayItem items[2] = {{m_cast, true}, {m_lit, true}};
    ::DrawOverlay(m_dev, m_overlayState, m_internalRefs, items, 2, 1);
}
