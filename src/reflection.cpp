#include "reflection.h"
#include "matrix.h"
#include "overlay.h"

#include <cmath>
#include <cstring>


void Reflection::NewFrame()
{
    m_waterSeen = false;
    m_cleared = false;
    m_frameRedraws = 0;
}

bool Reflection::EnsureResources()
{
    if (m_tex)
        return true;
    ULONG before = DeviceRefs(m_dev);
    UINT size = g_config.reflectionSize;
    HRESULT hr = m_dev->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_tex,
                                      nullptr);
    if (SUCCEEDED(hr))
        hr = m_tex->GetSurfaceLevel(0, &m_surf);
    if (SUCCEEDED(hr))
        hr = m_dev->CreateDepthStencilSurface(size, size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &m_depth, nullptr);
    if (FAILED(hr))
    {
        Log::Write("reflection: creating %ux%u target failed 0x%08x; pass disabled", size, size,
                   static_cast<unsigned>(hr));
        ReleaseResources();
        enabled = false;
        return false;
    }
    m_internalRefs = DeviceRefs(m_dev) - before;
    Log::Write("reflection: created %ux%u A8R8G8B8 target + D24S8 depth (tex %s), water height %g", size, size,
               Ptr(m_tex).c_str(), m_height);
    return true;
}

void Reflection::ReleaseResources()
{
    if (m_overlayState)
        m_overlayState->Release();
    if (m_surf)
        m_surf->Release();
    if (m_tex)
        m_tex->Release();
    if (m_depth)
        m_depth->Release();
    m_overlayState = nullptr;
    m_surf = nullptr;
    m_tex = nullptr;
    m_depth = nullptr;
    m_internalRefs = 0;
    m_cleared = false;
}

bool Reflection::TargetIsBackBuffer()
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

void Reflection::EnsureCleared()
{
    if (m_cleared || !EnsureResources())
        return;
    SavedTarget saved;
    saved.Save(m_dev);
    m_dev->SetRenderTarget(0, m_surf);
    m_dev->SetDepthStencilSurface(m_depth);
    m_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);  // alpha 0 = nothing reflected
    saved.Restore(m_dev);
    m_cleared = true;
}

void Reflection::BeforeDraw(const ShaderInfo* vs)
{
    if (!enabled || !needed || !vs || !vs->water || m_waterSeen)
        return;
    m_waterSeen = true;  // ships come before this; anything later (land units, wakes) is drawn after the water
    if (vs->world >= 0)
    {
        // Water mtxWorld is a translation to the plane height: row 2 = (0, 0, 1, height) in both games.
        float w[16];
        if (SUCCEEDED(m_dev->GetVertexShaderConstantF(static_cast<UINT>(vs->world), w, 3)) &&
            std::fabs(w[8]) < 1e-3f && std::fabs(w[9]) < 1e-3f && std::fabs(w[10] - 1.0f) < 1e-3f)
        {
            if (!m_loggedHeight || std::fabs(w[11] - m_height) > 1e-3f)
                Log::Write("reflection: water height %g (from water mtxWorld)", w[11]);
            m_loggedHeight = true;
            m_height = w[11];
        }
    }
    EnsureCleared();  // so a frame with no ships still shows an empty reflection, not last frame's
}

bool Reflection::AfterDraw(const ShaderInfo* vs, bool fixedFunction, const std::function<void()>& redraw)
{
    if (!enabled || !needed || m_waterSeen)
        return false;
    if (vs ? (vs->viewProj < 0 || vs->worldBones < 0) : !fixedFunction)
        return false;
    DWORD blend = D3DVBF_DISABLE;
    if (!vs)
    {
        m_dev->GetRenderState(D3DRS_VERTEXBLEND, &blend);
        if (blend == D3DVBF_DISABLE)
            return false;  // fixed-function draws other than skinned ones (blob shadows, effects) aren't reflected
        DWORD fvf = 0;
        m_dev->GetFVF(&fvf);
        if (!SkinnedLitFvf(fvf))
            return false;  // e.g. the unit selection circle, drawn while vertex blending is still on
    }
    if (!TargetIsBackBuffer())
        return false;  // e.g. the unit portrait rendered into a small texture for the interface
    EnsureCleared();
    if (!m_tex)
        return false;

    const float h = m_height;
    SavedTarget saved;
    saved.Save(m_dev);
    float savedPlane[4] = {};
    DWORD savedClip = 0;
    m_dev->GetClipPlane(0, savedPlane);
    m_dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &savedClip);

    m_dev->SetRenderTarget(0, m_surf);
    m_dev->SetDepthStencilSurface(m_depth);

    if (vs)
    {
        // Each output component is reg . (x, y, z, 1) with world-space x, y, z. Mirroring about z = h replaces z with
        // 2h - z: negate the z coefficient and add 2h * (old z coefficient) to w. Negating register 0 as well flips
        // clip-space x, which undoes the mirror's winding flip (Colonization does the same), so culling is unchanged.
        UINT reg = static_cast<UINT>(vs->viewProj);
        float vp[16], m[16];
        m_dev->GetVertexShaderConstantF(reg, vp, 4);
        mat::MirrorViewProj(vp, h, m);
        // Clip plane in clip space (programmable pipeline): keep world points with z >= h, i.e. the part of the ship
        // above the water, which is what appears in a reflection. Plane_clip = Plane_world * inverse(M).
        double inv[16];
        float plane[4] = {0, 0, 0, 0};
        if (mat::Invert(m, inv))
        {
            const double world[4] = {0, 0, 1, -h};
            for (int c = 0; c < 4; ++c)
                plane[c] = static_cast<float>(world[0] * inv[0 * 4 + c] + world[1] * inv[1 * 4 + c] +
                                              world[2] * inv[2 * 4 + c] + world[3] * inv[3 * 4 + c]);
            m_dev->SetClipPlane(0, plane);
            m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
        }
        m_dev->SetVertexShaderConstantF(reg, m, 4);
        redraw();
        m_dev->SetVertexShaderConstantF(reg, vp, 4);
    }
    else
    {
        // Fixed function (row vectors): world -> mirror -> view. Mirror about z = h is diag(1,1,-1,1) with translation
        // (0,0,2h), so View' = Mirror * View: row 2 negated, row 3 += 2h * row 2. The x flip goes into the projection
        // (column 0 negated). Fixed-function clip planes are in world space.
        D3DMATRIX view, proj, v, p;
        m_dev->GetTransform(D3DTS_VIEW, &view);
        m_dev->GetTransform(D3DTS_PROJECTION, &proj);
        v = view;
        for (int c = 0; c < 4; ++c)
        {
            v.m[2][c] = -view.m[2][c];
            v.m[3][c] = view.m[3][c] + 2.0f * h * view.m[2][c];
        }
        p = proj;
        for (int r = 0; r < 4; ++r)
            p.m[r][0] = -proj.m[r][0];
        const float plane[4] = {0, 0, 1, -h};
        m_dev->SetClipPlane(0, plane);
        m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
        m_dev->SetTransform(D3DTS_VIEW, &v);
        m_dev->SetTransform(D3DTS_PROJECTION, &p);
        redraw();
        m_dev->SetTransform(D3DTS_VIEW, &view);
        m_dev->SetTransform(D3DTS_PROJECTION, &proj);
    }

    m_dev->SetClipPlane(0, savedPlane);
    m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, savedClip);
    saved.Restore(m_dev);
    ++m_frameRedraws;
    return true;
}

void Reflection::DrawOverlay()
{
    if (!overlay || !m_tex)
        return;
    // left: colour; right: alpha (white = something reflected there)
    const OverlayItem items[2] = {{m_tex, false}, {m_tex, true}};
    ::DrawOverlay(m_dev, m_overlayState, m_internalRefs, items, 2, 0);
}

bool Reflection::SaveBmp(const std::wstring& path)
{
    return SaveSurfaceBmp(m_dev, m_surf, path);
}
