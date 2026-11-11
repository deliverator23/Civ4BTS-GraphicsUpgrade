#include "border_haze.h"

namespace
{
// The stencil bit a listed model's pixels are marked with (BtS draws with the stencil off; the bit is cleared again
// as the layer is laid over them).
const DWORD kMark = 0x80;

const D3DRENDERSTATETYPE kStencilStates[] = {D3DRS_STENCILENABLE, D3DRS_STENCILFUNC,      D3DRS_STENCILPASS,
                                             D3DRS_STENCILFAIL,   D3DRS_STENCILZFAIL,     D3DRS_STENCILREF,
                                             D3DRS_STENCILMASK,   D3DRS_STENCILWRITEMASK, D3DRS_TWOSIDEDSTENCILMODE};
const int kStencilCount = sizeof(kStencilStates) / sizeof(kStencilStates[0]);
}

bool BorderHaze::Listed()
{
    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(m_dev->GetTexture(0, &base)) || !base)
        return false;
    const bool listed = models.Contains(base);
    base->Release();
    return listed;
}

bool BorderHaze::EnsureTargets(IDirect3DSurface9* backBuffer)
{
    D3DSURFACE_DESC d = {};
    if (m_failed || FAILED(backBuffer->GetDesc(&d)))
        return false;
    if (m_layer && d.Width == m_desc.Width && d.Height == m_desc.Height && d.MultiSampleType == m_desc.MultiSampleType &&
        d.MultiSampleQuality == m_desc.MultiSampleQuality)
        return true;
    ReleaseDefault();
    const ULONG before = DeviceRefs(m_dev);
    if (FAILED(m_dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_A8R8G8B8, d.MultiSampleType, d.MultiSampleQuality,
                                         FALSE, &m_layer, nullptr)) ||
        FAILED(m_dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                    &m_resolved, nullptr)) ||
        FAILED(m_resolved->GetSurfaceLevel(0, &m_resolvedSurf)) || FAILED(m_dev->CreateStateBlock(D3DSBT_ALL, &m_state)))
    {
        m_internalRefs += DeviceRefs(m_dev) - before;
        ReleaseDefault();
        m_failed = true;
        Log::Write("borders: the tint layer (%ux%u, multisample %u) can't be made; ModelsUnderBorders stay over the "
                   "tint", d.Width, d.Height, d.MultiSampleType);
        return false;
    }
    m_internalRefs += DeviceRefs(m_dev) - before;
    m_desc = d;
    m_tinted = false;
    Log::Write("borders: tint layer %ux%u (multisample %u) for the models under the borders", d.Width, d.Height,
               d.MultiSampleType);
    return true;
}

void BorderHaze::AddBorder(const std::function<void()>& draw)
{
    IDirect3DSurface9* target = nullptr;
    if (FAILED(m_dev->GetRenderTarget(0, &target)) || !target)
        return;
    const bool ok = EnsureTargets(target);
    target->Release();
    if (!ok)
        return;
    SavedTarget saved;
    saved.Save(m_dev);
    m_dev->SetRenderTarget(0, m_layer);  // the same depth surface: the border hides behind the same ground
    if (!m_tinted)
    {
        m_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);  // whole target (the viewport is reset to it)
        m_tinted = true;
    }
    m_dev->SetViewport(&saved.viewport);
    // Premultiplied, over what's there: colour x alpha, and the alphas combined, so the layer laid over a model with
    // (one, 1 - alpha) gives what the border gave the ground.
    const D3DRENDERSTATETYPE states[] = {D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA,
                                         D3DRS_SRGBWRITEENABLE};
    const DWORD values[] = {TRUE, D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, FALSE};
    DWORD old[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        m_dev->GetRenderState(states[i], &old[i]);
        m_dev->SetRenderState(states[i], values[i]);
    }
    draw();
    for (int i = 0; i < 4; ++i)
        m_dev->SetRenderState(states[i], old[i]);
    saved.Restore(m_dev);
    m_resolvedFresh = false;
}

HRESULT BorderHaze::DrawUnder(const std::function<HRESULT()>& draw)
{
    IDirect3DSurface9* target = nullptr;
    D3DSURFACE_DESC d = {};
    if (!m_tinted || !m_layer || FAILED(m_dev->GetRenderTarget(0, &target)) || !target)
        return draw();
    const bool same = SUCCEEDED(target->GetDesc(&d)) && d.Width == m_desc.Width && d.Height == m_desc.Height &&
                      d.MultiSampleType == m_desc.MultiSampleType;
    if (!same)
    {
        target->Release();
        return draw();
    }
    // The model, its pixels marked. A blended one (Dune Wars' cave mouths, a soft shadow over the ground) can't be
    // marked everywhere it touches: its see-through fringe shows ground that already has the tint, and would get it
    // twice. Fixed-function, it takes the layer in proportion to its alpha (DrawWeighted); with a vertex shader, it is
    // drawn again with no colour, marking only where it's mostly opaque.
    DWORD old[kStencilCount] = {};
    for (int i = 0; i < kStencilCount; ++i)
        m_dev->GetRenderState(kStencilStates[i], &old[i]);
    const DWORD mark[kStencilCount] = {TRUE, D3DCMP_ALWAYS, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP,
                                       kMark, kMark, kMark, FALSE};
    DWORD blended = FALSE;
    m_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blended);
    IDirect3DVertexShader9* shader = nullptr;
    m_dev->GetVertexShader(&shader);
    if (shader)
        shader->Release();
    if (blended && !shader)
    {
        // Fixed-function and blended: drawn once more, taking the layer at each pixel in proportion to its own alpha
        // (no edge where the mouth fades into the ground).
        target->Release();
        const HRESULT drawn = draw();
        if (EnsureResolved())
            DrawWeighted(draw);
        return drawn;
    }
    HRESULT hr;
    if (blended)
    {
        hr = draw();
        const D3DRENDERSTATETYPE states[] = {D3DRS_COLORWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,
                                             D3DRS_ALPHAFUNC,        D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC};
        const DWORD values[] = {0, TRUE, 0x80, D3DCMP_GREATER, FALSE, D3DCMP_LESSEQUAL};
        DWORD oldStates[6] = {};
        for (int i = 0; i < 6; ++i)
        {
            m_dev->GetRenderState(states[i], &oldStates[i]);
            m_dev->SetRenderState(states[i], values[i]);
        }
        for (int i = 0; i < kStencilCount; ++i)
            m_dev->SetRenderState(kStencilStates[i], mark[i]);
        draw();
        for (int i = 0; i < 6; ++i)
            m_dev->SetRenderState(states[i], oldStates[i]);
    }
    else
    {
        for (int i = 0; i < kStencilCount; ++i)
            m_dev->SetRenderState(kStencilStates[i], mark[i]);
        hr = draw();
    }
    for (int i = 0; i < kStencilCount; ++i)
        m_dev->SetRenderState(kStencilStates[i], old[i]);

    // The layer over them (and the marks cleared).
    target->Release();
    if (!EnsureResolved())
        return hr;
    m_state->Capture();
    const D3DVIEWPORT9 full = {0, 0, m_desc.Width, m_desc.Height, 0.0f, 1.0f};
    m_dev->SetViewport(&full);
    m_dev->SetVertexShader(nullptr);
    m_dev->SetPixelShader(nullptr);
    m_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    m_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    m_dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    m_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    const DWORD lay[kStencilCount] = {TRUE, D3DCMP_EQUAL, D3DSTENCILOP_ZERO, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP,
                                      kMark, kMark, kMark, FALSE};
    for (int i = 0; i < kStencilCount; ++i)
        m_dev->SetRenderState(kStencilStates[i], lay[i]);
    m_dev->SetTexture(0, m_resolved);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    m_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    m_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    m_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    m_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    const float w = static_cast<float>(m_desc.Width), h = static_cast<float>(m_desc.Height);
    struct V
    {
        float x, y, z, rhw, u, v;
    } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0}, {-0.5f, h - 0.5f, 0, 1, 0, 1},
              {w - 0.5f, h - 0.5f, 0, 1, 1, 1}};
    m_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    m_state->Apply();
    return hr;
}

bool BorderHaze::EnsureResolved()
{
    if (m_resolvedFresh)
        return true;
    m_resolvedFresh = SUCCEEDED(m_dev->StretchRect(m_layer, nullptr, m_resolvedSurf, nullptr, D3DTEXF_NONE));
    if (!m_resolvedFresh && !m_logged)
    {
        m_logged = true;
        Log::Write("borders: the tint layer can't be read back (StretchRect failed); models stay over the tint");
    }
    return m_resolvedFresh;
}

void BorderHaze::DrawWeighted(const std::function<HRESULT()>& draw)
{
    // Stage 0 as the game set it (its texture's alpha, times whatever else it takes alpha from); stage 1 the layer, at
    // the pixel each point lands on: the camera-space position, through the projection, onto the screen. Out:
    // colour = layer x alpha, alpha = layer alpha x alpha, over the model with (one, 1 - alpha): the tint in
    // proportion to how much of the pixel is the model.
    D3DMATRIX proj = {}, oldTex = {};
    m_dev->GetTransform(D3DTS_PROJECTION, &proj);
    m_dev->GetTransform(D3DTS_TEXTURE1, &oldTex);
    const float bx = 0.5f + 0.5f / static_cast<float>(m_desc.Width), by = 0.5f + 0.5f / static_cast<float>(m_desc.Height);
    // clip (x, y, z, w) -> (0.5 x + bx w, -0.5 y + by w, 0, w), applied after the projection (row vectors)
    const float b[4][4] = {{0.5f, 0, 0, 0}, {0, -0.5f, 0, 0}, {0, 0, 0, 0}, {bx, by, 0, 1}};
    D3DMATRIX tex = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += proj.m[r][k] * b[k][c];
            tex.m[r][c] = s;
        }
    const D3DTEXTURESTAGESTATETYPE s1[] = {D3DTSS_COLOROP,       D3DTSS_COLORARG1,     D3DTSS_COLORARG2,
                                           D3DTSS_ALPHAOP,       D3DTSS_ALPHAARG1,     D3DTSS_ALPHAARG2,
                                           D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS};
    const DWORD v1[] = {D3DTOP_BLENDCURRENTALPHA, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTOP_MODULATE, D3DTA_TEXTURE,
                        D3DTA_CURRENT, D3DTSS_TCI_CAMERASPACEPOSITION, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED};
    const int n1 = sizeof(s1) / sizeof(s1[0]);
    DWORD old1[n1] = {}, old2[2] = {};
    for (int i = 0; i < n1; ++i)
    {
        m_dev->GetTextureStageState(1, s1[i], &old1[i]);
        m_dev->SetTextureStageState(1, s1[i], v1[i]);
    }
    m_dev->GetTextureStageState(2, D3DTSS_COLOROP, &old2[0]);
    m_dev->GetTextureStageState(2, D3DTSS_ALPHAOP, &old2[1]);
    m_dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    m_dev->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    const D3DSAMPLERSTATETYPE ss[] = {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                      D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE};
    const DWORD sv[] = {D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, FALSE};
    DWORD oldS[6] = {};
    for (int i = 0; i < 6; ++i)
    {
        m_dev->GetSamplerState(1, ss[i], &oldS[i]);
        m_dev->SetSamplerState(1, ss[i], sv[i]);
    }
    const D3DRENDERSTATETYPE rs[] = {D3DRS_SRCBLEND,         D3DRS_DESTBLEND,   D3DRS_BLENDOP,  D3DRS_SEPARATEALPHABLENDENABLE,
                                     D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC,       D3DRS_TEXTUREFACTOR, D3DRS_FOGENABLE,
                                     D3DRS_ALPHATESTENABLE};
    const DWORD rv[] = {D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE, D3DCMP_LESSEQUAL, 0, FALSE, FALSE};
    const int nr = sizeof(rs) / sizeof(rs[0]);
    DWORD oldR[nr] = {};
    for (int i = 0; i < nr; ++i)
    {
        m_dev->GetRenderState(rs[i], &oldR[i]);
        m_dev->SetRenderState(rs[i], rv[i]);
    }
    IDirect3DBaseTexture9* old1Tex = nullptr;
    m_dev->GetTexture(1, &old1Tex);
    m_dev->SetTexture(1, m_resolved);
    m_dev->SetTransform(D3DTS_TEXTURE1, &tex);
    draw();
    m_dev->SetTransform(D3DTS_TEXTURE1, &oldTex);
    m_dev->SetTexture(1, old1Tex);
    if (old1Tex)
        old1Tex->Release();
    for (int i = 0; i < nr; ++i)
        m_dev->SetRenderState(rs[i], oldR[i]);
    for (int i = 0; i < 6; ++i)
        m_dev->SetSamplerState(1, ss[i], oldS[i]);
    m_dev->SetTextureStageState(2, D3DTSS_COLOROP, old2[0]);
    m_dev->SetTextureStageState(2, D3DTSS_ALPHAOP, old2[1]);
    for (int i = 0; i < n1; ++i)
        m_dev->SetTextureStageState(1, s1[i], old1[i]);
}

void BorderHaze::ReleaseDefault()
{
    m_tinted = false;
    m_resolvedFresh = false;
    // Only touch the device while we hold something (this also runs from the destructor, after the device has gone).
    if (!m_layer && !m_resolved && !m_resolvedSurf && !m_state)
        return;
    const ULONG before = DeviceRefs(m_dev);
    IUnknown* objects[] = {m_resolvedSurf, m_resolved, m_layer, m_state};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_resolvedSurf = nullptr;
    m_resolved = nullptr;
    m_layer = nullptr;
    m_state = nullptr;
    const ULONG after = DeviceRefs(m_dev);
    m_internalRefs -= (before > after && m_internalRefs >= before - after) ? before - after : 0;
}
