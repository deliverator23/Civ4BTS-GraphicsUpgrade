#include "area_borders.h"
#include "matrix.h"

#include <cstring>

namespace
{
// How far above the ground the draped vertices sit, and how far towards the camera they're moved (world units; a
// plot is 180 across): as the culture borders (borders.cpp), whose grid is the same 11.25 units.
const float kLift = 1.5f;
const float kPull = 24.0f;

enum
{
    kLogDrawn = 1,
    kLogNoHeights = 2,
    kLogFormat = 4,
    kLogBuffers = 8,
    kLogCamera = 16,
    kLogTooBig = 32,
};

// Row-vector (Direct3D) matrices as 16 floats.
void ToFloats(const D3DMATRIX& m, float* out)
{
    memcpy(out, &m, 16 * sizeof(float));
}
}  // namespace

void AreaBorders::LogOnce(int reason, const char* text)
{
    if (m_logged & reason)
        return;
    m_logged |= reason;
    Log::Write("area borders: %s", text);
}

bool AreaBorders::Listed()
{
    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(m_dev->GetTexture(0, &base)) || !base)
        return false;
    const bool listed = textures.Contains(base);
    base->Release();
    return listed;
}

bool AreaBorders::Draw(const TerrainNormals& terrain, D3DPRIMITIVETYPE type, INT base, UINT minVertex,
                       UINT numVertices, UINT start, UINT count, HRESULT& hr)
{
    if (type != D3DPT_TRIANGLELIST || count == 0 || numVertices == 0 || count > 65536 || numVertices > 65536)
    {
        LogOnce(kLogFormat, "a listed draw that isn't a small indexed triangle list: drawn as the game draws it");
        return false;
    }
    DWORD fvf = 0, blend = D3DVBF_DISABLE;
    m_dev->GetFVF(&fvf);
    m_dev->GetRenderState(D3DRS_VERTEXBLEND, &blend);
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(m_dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb)
        return false;
    Drape::Layout layout;
    if (blend != D3DVBF_DISABLE || !Drape::LayoutFor(fvf, stride, layout))
    {
        vb->Release();
        LogOnce(kLogFormat, "a listed draw whose vertices aren't plain positions (FVF) or are blended: drawn as the "
                            "game draws it");
        return false;
    }
    // The vertices the draw uses (index + base, from minVertex on) and its indices, both read back.
    D3DVERTEXBUFFER_DESC vd = {};
    vb->GetDesc(&vd);
    const INT firstVertex = base + static_cast<INT>(minVertex);
    const UINT from = offset + static_cast<UINT>(firstVertex) * stride, bytes = numVertices * stride;
    void* data = nullptr;
    bool read = firstVertex >= 0 && vd.Pool != D3DPOOL_DEFAULT && from + bytes <= vd.Size &&
                SUCCEEDED(vb->Lock(from, bytes, &data, D3DLOCK_READONLY));
    if (read)
    {
        m_source.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
        vb->Unlock();
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (read && (FAILED(m_dev->GetIndices(&ib)) || !ib))
        read = false;
    if (read)
    {
        D3DINDEXBUFFER_DESC id = {};
        ib->GetDesc(&id);
        const UINT size = id.Format == D3DFMT_INDEX32 ? 4 : 2, n = count * 3;
        void* idx = nullptr;
        read = id.Pool != D3DPOOL_DEFAULT && (start + n) * size <= id.Size &&
               SUCCEEDED(ib->Lock(start * size, n * size, &idx, D3DLOCK_READONLY));
        if (read)
        {
            m_tris.resize(n);
            for (UINT k = 0; k < n && read; ++k)
            {
                const uint32_t i = size == 4 ? static_cast<const uint32_t*>(idx)[k] : static_cast<const uint16_t*>(idx)[k];
                read = i >= minVertex && i - minVertex < numVertices;
                m_tris[k] = i - minVertex;
            }
            ib->Unlock();
        }
    }
    if (!read)
    {
        if (ib)
            ib->Release();
        vb->Release();
        LogOnce(kLogBuffers, "a listed draw whose buffers can't be read back: drawn as the game draws it");
        return false;
    }
    // Into the world (the placement applied to positions and normals), and the camera there.
    D3DMATRIX world, view, proj;
    m_dev->GetTransform(D3DTS_WORLD, &world);
    m_dev->GetTransform(D3DTS_VIEW, &view);
    m_dev->GetTransform(D3DTS_PROJECTION, &proj);
    float w[16], v[16], p[16], toClip[16], fromClip[16];
    ToFloats(world, w);
    ToFloats(view, v);
    ToFloats(proj, p);
    mat::Mul(v, p, toClip);  // row vectors: world, then view, then projection
    if (!mat::Invert(toClip, fromClip))
    {
        ib->Release();
        vb->Release();
        LogOnce(kLogCamera, "a listed draw with no usable camera: drawn as the game draws it");
        return false;
    }
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (UINT k = 0; k < numVertices; ++k)
    {
        uint8_t* vert = &m_source[static_cast<size_t>(k) * stride];
        float pos[3], out[4];
        memcpy(pos, vert, 12);
        Drape::Transform(pos, w, out);
        memcpy(vert, out, 12);
        if (layout.normalAt)
        {
            float n[3], t[3];
            memcpy(n, vert + layout.normalAt, 12);
            for (int c = 0; c < 3; ++c)
                t[c] = n[0] * w[c] + n[1] * w[4 + c] + n[2] * w[8 + c];
            memcpy(vert + layout.normalAt, t, 12);
        }
    }
    // The ground under the triangles that are drawn (those with any area: Drape::HasArea).
    for (size_t t = 0; t + 2 < m_tris.size(); t += 3)
    {
        float a[3], b[3], c[3];
        memcpy(a, &m_source[static_cast<size_t>(m_tris[t]) * stride], 12);
        memcpy(b, &m_source[static_cast<size_t>(m_tris[t + 1]) * stride], 12);
        memcpy(c, &m_source[static_cast<size_t>(m_tris[t + 2]) * stride], 12);
        if (!Drape::HasArea(a, b, c))
            continue;
        for (const float* q : {a, b, c})
        {
            x0 = (std::min)(x0, q[0]), y0 = (std::min)(y0, q[1]);
            x1 = (std::max)(x1, q[0]), y1 = (std::max)(y1, q[1]);
        }
    }
    if (!(x1 >= x0))
    {
        ib->Release();
        vb->Release();
        return false;  // nothing with any area: the game's draw shows nothing either
    }
    Drape::Heights heights;
    bool built = false, tooBig = false;
    if (terrain.DrapeHeights(x0, y0, x1, y1, heights))
    {
        built = Drape::Build(layout, m_source.data(), numVertices, m_tris, terrain.Spacing(), heights, kLift, toClip,
                             fromClip, kPull, m_verts, m_indices, m_stats);
        tooBig = !built && m_stats.tooMany;
    }
    else
        tooBig = terrain.Spacing() > 0;  // a grid, but the draw covers more than DrapeHeights takes
    if (!built)
    {
        ib->Release();
        vb->Release();
        if (tooBig)
            LogOnce(kLogTooBig, "a listed draw too big to drape: drawn as the game draws it");
        else
            LogOnce(kLogNoHeights, "a listed draw over ground whose heights haven't been read: drawn as the game "
                                   "draws it");
        return false;
    }

    // Drawn from our copy: in the world as it is, depth tested (ground in front hides it), no depth written (units drawn
    // after it still go over it). DrawIndexedPrimitiveUP unsets the game's buffers: put back after.
    DWORD zEnable = 0, zWrite = 0, zFunc = 0;
    m_dev->GetRenderState(D3DRS_ZENABLE, &zEnable);
    m_dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
    m_dev->GetRenderState(D3DRS_ZFUNC, &zFunc);
    m_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1;
    m_dev->SetTransform(D3DTS_WORLD, &identity);
    hr = m_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, m_stats.vertices, m_stats.trianglesOut, m_indices.data(),
                                       D3DFMT_INDEX16, m_verts.data(), stride);
    m_dev->SetTransform(D3DTS_WORLD, &world);
    m_dev->SetRenderState(D3DRS_ZENABLE, zEnable);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
    m_dev->SetRenderState(D3DRS_ZFUNC, zFunc);
    m_dev->SetStreamSource(0, vb, offset, stride);
    m_dev->SetIndices(ib);
    ib->Release();
    vb->Release();
    if (!(m_logged & kLogDrawn))
    {
        char text[200];
        snprintf(text, sizeof(text),
                 "draped over the terrain (%u triangles cut into %u, ground %.1f to %.1f), %.0f units nearer the camera",
                 m_stats.trianglesIn, m_stats.trianglesOut, m_stats.lowest, m_stats.highest, kPull);
        LogOnce(kLogDrawn, text);
    }
    return true;
}
