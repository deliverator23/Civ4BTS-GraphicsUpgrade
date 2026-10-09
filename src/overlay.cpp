#include "overlay.h"

#include <cstring>

void DrawOverlay(IDirect3DDevice9* dev, IDirect3DStateBlock9*& state, ULONG& internalRefs, const OverlayItem* items,
                 int count, int row)
{
    if (!state)
    {
        ULONG before = DeviceRefs(dev);
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &state)))
            return;
        internalRefs += DeviceRefs(dev) - before;
    }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    SavedTarget saved;  // state blocks don't include the render target
    saved.Save(dev);
    state->Capture();
    D3DSURFACE_DESC d = {};
    bb->GetDesc(&d);
    dev->SetRenderTarget(0, bb);
    bb->Release();

    if (SUCCEEDED(dev->BeginScene()))
    {
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

        const float s = 256.0f, top = 16.0f + row * (s + 8.0f);
        for (int i = 0; i < count; ++i)
        {
            float x0 = static_cast<float>(d.Width) - (count - i) * (s + 8.0f) - 8.0f;
            dev->SetTexture(0, items[i].texture);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1,
                                      items[i].alpha ? (D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE) : D3DTA_TEXTURE);
            struct V
            {
                float x, y, z, rhw, u, v;
            } q[4] = {{x0 - 0.5f, top - 0.5f, 0, 1, 0, 0},
                      {x0 + s - 0.5f, top - 0.5f, 0, 1, 1, 0},
                      {x0 - 0.5f, top + s - 0.5f, 0, 1, 0, 1},
                      {x0 + s - 0.5f, top + s - 0.5f, 0, 1, 1, 1}};
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        }
        dev->EndScene();
    }
    state->Apply();
    saved.Restore(dev);
}

namespace
{
// 32-bit BMP, top-down (negative height), BGRA with alpha in the fourth byte.
bool WriteBmp(const std::wstring& path, const D3DLOCKED_RECT& lr, UINT width, UINT height)
{
    const size_t row = static_cast<size_t>(width) * 4;
    std::string file(54 + row * height, '\0');
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&file[at], &v, 4); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<uint32_t>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(height)));
    put32(26, 1 | (32 << 16));  // planes, bits per pixel
    put32(34, static_cast<uint32_t>(row * height));
    for (UINT y = 0; y < height; ++y)
        memcpy(&file[54 + y * row], static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch, row);
    return WriteBytes(path, file.data(), file.size());
}
}

bool SaveTextureBmp(IDirect3DBaseTexture9* base, const std::wstring& path)
{
    if (!base || base->GetType() != D3DRTYPE_TEXTURE)
        return false;
    auto* tex = static_cast<IDirect3DTexture9*>(base);
    D3DSURFACE_DESC d = {};
    D3DLOCKED_RECT lr;
    if (FAILED(tex->GetLevelDesc(0, &d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) ||
        d.Pool == D3DPOOL_DEFAULT || FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
        return false;
    bool ok = WriteBmp(path, lr, d.Width, d.Height);
    tex->UnlockRect(0);
    return ok;
}

bool SaveSurfaceBmp(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const std::wstring& path)
{
    if (!surface)
        return false;
    D3DSURFACE_DESC d = {};
    surface->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
        return false;
    bool ok = false;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(dev->GetRenderTargetData(surface, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    {
        ok = WriteBmp(path, lr, d.Width, d.Height);
        sys->UnlockRect();
    }
    sys->Release();
    return ok;
}
