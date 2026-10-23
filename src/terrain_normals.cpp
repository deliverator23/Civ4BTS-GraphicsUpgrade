#include "terrain_normals.h"
#include "overlay.h"
#include "perf.h"
#include "hill_shadow_ps.h"
#include "hill_shadow_vs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

void TerrainNormals::Capture(const ShaderInfo& vs)
{
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(m_dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb)
        return;
    // mtxWorld: three register rows (float4x3, translation in .w). A buffer is read once in each place it's drawn.
    float w[12];
    m_dev->GetVertexShaderConstantF(static_cast<UINT>(vs.world), w, 3);
    const std::array<float, 3> place = {w[3], w[7], w[11]};
    auto& places = m_done[vb];
    bool seen = false;
    for (const auto& at : places)
        seen = seen || (std::fabs(at[0] - place[0]) < 0.5f && std::fabs(at[1] - place[1]) < 0.5f &&
                        std::fabs(at[2] - place[2]) < 0.5f);
    if (seen || stride < 12)
    {
        vb->Release();
        return;
    }
    places.push_back(place);
    const double readStart = Perf::Now();
    D3DVERTEXBUFFER_DESC d = {};
    vb->GetDesc(&d);
    void* data = nullptr;
    if (FAILED(vb->Lock(0, 0, &data, D3DLOCK_READONLY)) || !data)
    {
        ++m_unreadable;
        if (!m_loggedUnreadable)
        {
            m_loggedUnreadable = true;
            Log::Write("terrain normals: terrain vertex buffer can't be read (pool %u, usage 0x%x, fvf 0x%x, %u bytes); "
                       "no per-pixel terrain lighting", d.Pool, d.Usage, d.FVF, d.Size);
        }
        vb->Release();
        return;
    }
    const UINT count = (d.Size - offset) / stride;
    const BYTE* base = static_cast<const BYTE*>(data) + offset;
    std::vector<float> world(count * 3);
    for (UINT i = 0; i < count; ++i)
    {
        const float* p = reinterpret_cast<const float*>(base + i * stride);
        for (int r = 0; r < 3; ++r)
            world[i * 3 + r] = w[r * 4] * p[0] + w[r * 4 + 1] * p[1] + w[r * 4 + 2] * p[2] + w[r * 4 + 3];
    }
    if (!m_haveDetailAxes)
        FitDetailAxes(base, count, stride, world);
    vb->Unlock();

    if (m_spacing <= 0)
    {
        // Grid spacing: the smallest positive step between distinct x values of the first buffer.
        std::vector<float> xs;
        for (UINT i = 0; i < count; ++i)
            xs.push_back(world[i * 3]);
        std::sort(xs.begin(), xs.end());
        float step = 0;
        for (size_t i = 1; i < xs.size(); ++i)
            if (xs[i] - xs[i - 1] > 0.5f && (step == 0 || xs[i] - xs[i - 1] < step))
                step = xs[i] - xs[i - 1];
        if (step <= 0)
        {
            vb->Release();
            return;
        }
        m_vertexSpacing = step;
        const float span = MapSpan(vs);
        m_spacing = GridSpacingFor(step, span);
        m_cells = CellsFor(m_spacing, span);
        D3DCAPS9 caps = {};
        while (m_cells > kMinCells && SUCCEEDED(m_dev->GetDeviceCaps(&caps)) &&
               (caps.MaxTextureWidth < m_cells || caps.MaxTextureHeight < m_cells))
            m_cells /= 2;
        m_heights.assign(m_cells * m_cells, std::numeric_limits<float>::quiet_NaN());
        m_normals.assign(m_cells * m_cells, 0x00808080);
        char every[64] = "";
        if (m_spacing > step * 1.5f)
            snprintf(every, sizeof(every), " (1 in %d of the vertices, %.3f apart)", static_cast<int>(m_spacing / step + 0.5f),
                     step);
        Log::Write("terrain normals: first terrain vertex buffer: pool %u, usage 0x%x, fvf 0x%x, stride %u, %u vertices; "
                   "grid spacing %.3f world units%s (%u cells cover %.0f units, wrapping; map %.0f units across)",
                   d.Pool, d.Usage, d.FVF, stride, count, m_spacing, every, m_cells, m_spacing * m_cells, span);
        if (span > m_spacing * m_cells + m_spacing)
            Log::Write("terrain normals: the map is wider than the grid; its far side shares cells with its near side "
                       "(terrain lighting and hill shadows may be wrong there)");
    }
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    unsigned offGrid = 0;
    for (UINT i = 0; i < count; ++i)
    {
        float fx = world[i * 3] / m_spacing, fy = world[i * 3 + 1] / m_spacing;
        int cx = static_cast<int>(std::floor(fx + 0.5f)), cy = static_cast<int>(std::floor(fy + 0.5f));
        if (std::fabs(fx - cx) > 0.05f || std::fabs(fy - cy) > 0.05f)
        {
            // Between the grid's cells: a vertex the grid skips (it keeps every 2nd, 4th... at fine spacings), or
            // off BtS's own vertex grid altogether (which shouldn't happen).
            const float vx = world[i * 3] / m_vertexSpacing, vy = world[i * 3 + 1] / m_vertexSpacing;
            if (std::fabs(vx - std::floor(vx + 0.5f)) > 0.05f || std::fabs(vy - std::floor(vy + 0.5f)) > 0.05f)
                ++offGrid;
            continue;
        }
        Height(cx, cy) = world[i * 3 + 2];
        m_minHeight = (std::min)(m_minHeight, world[i * 3 + 2]);
        m_maxHeight = (std::max)(m_maxHeight, world[i * 3 + 2]);
        x0 = (std::min)(x0, cx), y0 = (std::min)(y0, cy), x1 = (std::max)(x1, cx), y1 = (std::max)(y1, cy);
    }
    ++m_buffers;
    m_vertices += count;
    if (offGrid)
        Log::Write("terrain normals: %u of %u vertices off the %.3f grid in buffer %u", offGrid, count, m_spacing, m_buffers);
    if (x1 >= x0)
    {
        UpdateNormals(x0 - 1, y0 - 1, x1 + 1, y1 + 1);
        m_heightsDirty = true;            // hill shadows: worked out again
        m_heightsNew.Add(x0, y0, x1, y1);  // these heights uploaded
        m_shoreNew.Add(x0, y0, x1, y1);    // and the shore worked out again around them
        m_read.Add(x0, y0, x1, y1);
    }
    vb->Release();
    m_readSeconds += Perf::Now() - readStart;
}

template <class T, class Fill>
bool TerrainNormals::Upload(IDirect3DTexture9* tex, const TerrainCells::Rect& r, Fill fill)
{
    const double start = Perf::Now();
    bool ok = true;
    TerrainCells::ForEachPiece(r, static_cast<int>(m_cells), [&](int gx0, int gy0, int gx1, int gy1, int tx, int ty) {
        RECT rc = {tx, ty, tx + gx1 - gx0 + 1, ty + gy1 - gy0 + 1};
        D3DLOCKED_RECT lr;
        if (!ok || FAILED(tex->LockRect(0, &lr, &rc, 0)))
        {
            ok = false;
            return;
        }
        for (int y = gy0; y <= gy1; ++y)
        {
            T* row = reinterpret_cast<T*>(static_cast<BYTE*>(lr.pBits) + (y - gy0) * lr.Pitch);
            for (int x = gx0; x <= gx1; ++x)
                row[x - gx0] = fill(x, y);
        }
        tex->UnlockRect(0);
        m_uploadBytes += static_cast<double>(gx1 - gx0 + 1) * (gy1 - gy0 + 1) * sizeof(T);
    });
    m_uploadSeconds += Perf::Now() - start;
    return ok;
}

float TerrainNormals::GridSpacingFor(float vertexSpacing, float mapSpan)
{
    float spacing = vertexSpacing;
    while (spacing * kMaxCells < (std::max)(mapSpan, kHugeMapSpan) - 0.01f * spacing)
        spacing *= 2;
    return spacing;
}

UINT TerrainNormals::CellsFor(float spacing, float mapSpan)
{
    const float cover = (std::max)(mapSpan, kHugeMapSpan) / spacing;
    UINT cells = kMinCells;
    while (cells < kMaxCells && cells < cover - 0.01f)  // (a map exactly as wide as the grid fits: its edges meet)
        cells *= 2;
    return cells;
}

float TerrainNormals::MapSpan(const ShaderInfo& vs) const
{
    // mtxLightmap (float4x2, a register per column): the lightmap's uv from world xy, over the whole map. Its width
    // is 1 / (uv per world unit) along each axis; BtS's lightmap is square, over the map's longer side.
    const int reg = vs.Reg("mtxLightmap", 2);
    float m[8];
    if (reg < 0 || FAILED(m_dev->GetVertexShaderConstantF(static_cast<UINT>(reg), m, 2)))
        return 0;
    const float perX = std::hypot(m[0], m[4]), perY = std::hypot(m[1], m[5]);
    float span = 0;
    for (float per : {perX, perY})
        if (per > 1e-7f && per < 1e-2f)  // a sane map: 100 to 10 million units across
            span = (std::max)(span, 1 / per);
    return span;
}

void TerrainNormals::FitDetailAxes(const BYTE* vertices, UINT count, UINT stride, const std::vector<float>& world)
{
    // Where TEXCOORD1 (the detail texture's uv, before BtS's fDetailTexScaling) is in a vertex.
    IDirect3DVertexDeclaration9* decl = nullptr;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT n = 0;
    int offset = -1;
    if (SUCCEEDED(m_dev->GetVertexDeclaration(&decl)) && decl && SUCCEEDED(decl->GetDeclaration(elements, &n)))
        for (UINT i = 0; i < n && elements[i].Stream != 0xFF; ++i)
            if (elements[i].Stream == 0 && elements[i].Usage == D3DDECLUSAGE_TEXCOORD && elements[i].UsageIndex == 1 &&
                elements[i].Type == D3DDECLTYPE_FLOAT2)
                offset = elements[i].Offset;
    if (decl)
        decl->Release();
    DWORD fvf = 0;
    if (offset < 0 && SUCCEEDED(m_dev->GetFVF(&fvf)) && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ &&
        ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) >= 2 && !(fvf & 0xFFFF0000))  // two-float uvs
        offset = 12 + ((fvf & D3DFVF_NORMAL) ? 12 : 0) + ((fvf & D3DFVF_PSIZE) ? 4 : 0) +
                 ((fvf & D3DFVF_DIFFUSE) ? 4 : 0) + ((fvf & D3DFVF_SPECULAR) ? 4 : 0) + 8;
    if (offset < 0 || static_cast<UINT>(offset) + 8 > stride || count < 3)
    {
        if (!m_loggedNoDetailUv)
            Log::Write("terrain normals: no detail uv (TEXCOORD1) in the terrain's vertices; no detail normals");
        m_loggedNoDetailUv = true;
        return;
    }
    // Least squares, u and v each = a x + b y + c over the buffer's vertices (x, y relative to the first one).
    double m[3][3] = {}, ru[3] = {}, rv[3] = {};
    for (UINT i = 0; i < count; ++i)
    {
        const float* uv = reinterpret_cast<const float*>(vertices + i * stride + offset);
        const double row[3] = {world[i * 3] - world[0], world[i * 3 + 1] - world[1], 1.0};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                m[r][c] += row[r] * row[c];
            ru[r] += row[r] * uv[0];
            rv[r] += row[r] * uv[1];
        }
    }
    auto solve = [&](const double* rhs, double* out) {
        double a[3][4];
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                a[r][c] = m[r][c];
            a[r][3] = rhs[r];
        }
        for (int c = 0; c < 3; ++c)
        {
            int pivot = c;
            for (int r = c + 1; r < 3; ++r)
                if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                    pivot = r;
            if (std::fabs(a[pivot][c]) < 1e-9)
                return false;
            for (int k = 0; k < 4; ++k)
                std::swap(a[c][k], a[pivot][k]);
            for (int r = 0; r < 3; ++r)
                if (r != c)
                {
                    const double f = a[r][c] / a[c][c];
                    for (int k = 0; k < 4; ++k)
                        a[r][k] -= f * a[c][k];
                }
        }
        for (int r = 0; r < 3; ++r)
            out[r] = a[r][3] / a[r][r];
        return true;
    };
    double fu[3], fv[3];
    if (!solve(ru, fu) || !solve(rv, fv))
        return;
    const double lu = std::hypot(fu[0], fu[1]), lv = std::hypot(fv[0], fv[1]);
    if (lu < 1e-9 || lv < 1e-9)
        return;
    m_detailAxes[0] = static_cast<float>(fu[0] / lu), m_detailAxes[1] = static_cast<float>(fu[1] / lu);
    m_detailAxes[2] = static_cast<float>(fv[0] / lv), m_detailAxes[3] = static_cast<float>(fv[1] / lv);
    m_haveDetailAxes = true;
    Log::Write("terrain normals: detail texture uv per world unit: u = %.5f x + %.5f y, v = %.5f x + %.5f y",
               fu[0], fu[1], fv[0], fv[1]);
}

IDirect3DTexture9* TerrainNormals::ShoreTexture(float waterHeight)
{
    if (m_spacing <= 0 || m_read.Empty())
        return nullptr;
    if (!m_shoreTex)
    {
        ULONG before = DeviceRefs(m_dev);
        if (FAILED(m_dev->CreateTexture(m_cells, m_cells, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_shoreTex, nullptr)))
        {
            m_shoreTex = nullptr;
            return nullptr;
        }
        m_internalRefs += DeviceRefs(m_dev) - before;
        Upload<DWORD>(m_shoreTex, TerrainCells::Rect::Whole(m_cells), [](int, int) { return 0u; });  // unknown
        m_shoreAll = true;
    }
    // New terrain: the shore around it; a new texture or water height: around all the terrain read.
    const bool all = m_shoreAll || waterHeight != m_shoreHeight;
    const TerrainCells::Rect changed = all ? m_read : m_shoreNew;
    if (changed.Empty())
        return m_shoreTex;
    TerrainCells::Rect write, window;
    TerrainCells::ShoreRects(changed, static_cast<int>(m_cells), write, window);
    const double start = Perf::Now();
    const std::vector<float> dist =
        TerrainCells::ShoreDistances(window, waterHeight, [&](int x, int y) { return Height(x, y); });
    m_uploadSeconds += Perf::Now() - start;
    const int w = window.x1 - window.x0 + 1;
    Upload<DWORD>(m_shoreTex, write, [&](int x, int y) {
        return static_cast<DWORD>(TerrainCells::ShoreTexel(
            dist[static_cast<size_t>(y - window.y0) * w + (x - window.x0)], Height(x, y), waterHeight));
    });
    m_shoreNew = TerrainCells::Rect();
    m_shoreAll = false;
    m_shoreHeight = waterHeight;
    return m_shoreTex;
}

bool TerrainNormals::EnsureHeightTexture()
{
    if (!m_heightTex)
    {
        ULONG before = DeviceRefs(m_dev);
        if (FAILED(m_dev->CreateTexture(m_cells, m_cells, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &m_heightTex, nullptr)))
        {
            m_heightTex = nullptr;
            return false;
        }
        // Bilinear heights where the card can filter R32F (any recent one), else nearest.
        IDirect3D9* d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        D3DDISPLAYMODE mode = {};
        if (SUCCEEDED(m_dev->GetDirect3D(&d3d)) && SUCCEEDED(m_dev->GetCreationParameters(&cp)) &&
            SUCCEEDED(m_dev->GetDisplayMode(0, &mode)))
            m_heightFilter = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                              D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_R32F));
        if (d3d)
            d3d->Release();
        m_internalRefs += DeviceRefs(m_dev) - before;
        m_heightsNew = TerrainCells::Rect::Whole(m_cells);
    }
    if (!m_heightsNew.Empty() && !m_heights.empty() && Upload<float>(m_heightTex, m_heightsNew, [&](int x, int y) {
            const float v = Height(x, y);
            return std::isnan(v) ? -10000.0f : v;
        }))
        m_heightsNew = TerrainCells::Rect();
    return true;
}

bool TerrainNormals::EnsureHillResources()
{
    if (m_hillFailed)
        return false;
    if (m_hillTex && m_heightTex && m_hillPs)
        return true;
    if (!EnsureHeightTexture())
    {
        m_hillFailed = true;
        Log::Write("terrain: hill shadows need an R32F texture; creating it failed, so no hill shadows");
        return false;
    }
    ULONG before = DeviceRefs(m_dev);
    auto tokens = [](const BYTE* code, size_t size) {
        std::vector<DWORD> t(size / sizeof(DWORD));
        memcpy(t.data(), code, t.size() * sizeof(DWORD));
        return t;
    };
    bool ok = true;
    if (!m_hillVs)
        ok = SUCCEEDED(m_dev->CreateVertexShader(tokens(g_hillShadowVS, sizeof(g_hillShadowVS)).data(), &m_hillVs));
    if (ok && !m_hillPs)
        ok = SUCCEEDED(m_dev->CreatePixelShader(tokens(g_hillShadowPS, sizeof(g_hillShadowPS)).data(), &m_hillPs));
    if (ok && !m_hillTex)
        ok = SUCCEEDED(m_dev->CreateTexture(kHillCells, kHillCells, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                            &m_hillTex, nullptr)) &&
             SUCCEEDED(m_hillTex->GetSurfaceLevel(0, &m_hillSurf));
    if (ok && !m_hillState)
        ok = SUCCEEDED(m_dev->CreateStateBlock(D3DSBT_ALL, &m_hillState));
    m_internalRefs += DeviceRefs(m_dev) - before;
    if (!ok)
    {
        m_hillFailed = true;
        Log::Write("terrain: hill shadows need Shader Model 3 and an R32F texture; creating them failed, so no hill shadows");
    }
    return ok;
}

bool TerrainNormals::UpdateHills(const float* sunDirection, float darkness, const float* view)
{
    if (m_spacing <= 0 || !EnsureHillResources())
        return false;
    float sun[3] = {sunDirection[0], sunDirection[1], sunDirection[2]};
    float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    for (float& v : sun)
        v /= len > 0 ? len : 1;
    // The area: a square on the ground snapped to whole texels (and its size to 64 units), so panning doesn't make the
    // edges shimmer: the ground in view and a quarter more, kept while the view stays inside it and isn't under half
    // its size (zoomed in: too coarse). Small pans and the view's own wobble from frame to frame (it comes from the
    // first draw with a camera, a terrain or a unit draw, and their matrices differ slightly) then don't make a new
    // mask. Before any view is known, the whole grid.
    float region[3];
    bool viewMoved;
    if (view[2] > 0)
    {
        const float need = view[2] * 1.05f + 200.0f;  // the half-width the view needs covered
        const float off = (std::max)(std::fabs(view[0] - m_hillRegion[0]), std::fabs(view[1] - m_hillRegion[1]));
        viewMoved = off + need > m_hillRegion[2] || need < 0.5f * m_hillRegion[2];
        if (viewMoved)
        {
            region[2] = std::ceil(need * 1.25f / 64.0f) * 64.0f;
            const float texel = 2 * region[2] / kHillCells;
            region[0] = std::floor(view[0] / texel) * texel;
            region[1] = std::floor(view[1] / texel) * texel;
        }
        else
            memcpy(region, m_hillRegion, sizeof(region));
    }
    else
    {
        region[0] = region[1] = region[2] = 0.5f * m_spacing * m_cells;
        viewMoved = memcmp(region, m_hillRegion, sizeof(region)) != 0;
    }
    // Made again only for something to see: new terrain, the view leaving the area, the sun moving more than 0.1
    // degrees or the darkness half a colour step (the light cycle moves both a little every frame; a 0.1 degree turn
    // moves even a long hill shadow's tip by well under a texel; at its default speed, a few new masks a second).
    const float kSunStill = 0.99999848f;  // cos 0.1 degrees
    int why = 0;
    if (!m_hillReady)
        why = kHillFirst;
    else
    {
        why |= m_heightsDirty ? kHillTerrain : 0;
        why |= viewMoved ? kHillView : 0;
        why |= sun[0] * m_hillSun[0] + sun[1] * m_hillSun[1] + sun[2] * m_hillSun[2] > kSunStill ? 0 : kHillSun;
        why |= std::fabs(darkness - m_hillDarkness) < 0.5f / 255 ? 0 : kHillDarkness;
    }
    if (!why)
        return false;
    m_hillWhy = why;
    if (m_heightsDirty)
    {
        EnsureHeightTexture();  // the new heights uploaded (unless the waves' draping already has)
        m_heightsDirty = false;
    }
    // Half a cell per step towards the sun (so a one-vertex ridge isn't stepped over); the beam rises by the sun's
    // slope per step, divided by HillShadowLength, over as many steps as it takes to climb the terrain's height
    // range (at most 192).
    const float tx = -sun[0], ty = -sun[1], tz = -sun[2];
    const float hor = std::sqrt(tx * tx + ty * ty);
    float step[4] = {0, 0, 0, 0};
    if (hor > 1e-4f && tz > 0)
    {
        const float rise = 0.5f * m_spacing * tz / hor / g_config.hillShadowLength;
        step[0] = 0.5f * tx / hor / m_cells, step[1] = 0.5f * ty / hor / m_cells, step[2] = rise;
        step[3] = (std::min)(192.0f, std::ceil((m_maxHeight - m_minHeight) / rise) + 1);
    }
    // Soft edges: HillShadowSoftness at the foot of the slope, widening by 3% of the distance from it (a sun's
    // penumbra), so long shadows fade out at their tips and small bumps only cast faint shade.
    const float shade[4] = {(std::max)(0.01f, g_config.hillShadowSoftness), darkness, m_maxHeight, 0.015f * m_spacing};
    const float known[4] = {m_minHeight - 1.0f, 2.0f, 0, 0};

    SavedTarget saved;
    saved.Save(m_dev);
    m_hillState->Capture();
    m_dev->SetRenderTarget(0, m_hillSurf);
    m_dev->SetDepthStencilSurface(nullptr);
    if (SUCCEEDED(m_dev->BeginScene()))
    {
        m_dev->SetVertexShader(m_hillVs);
        m_dev->SetPixelShader(m_hillPs);
        m_dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
        const D3DRENDERSTATETYPE off[] = {D3DRS_ZENABLE,       D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                                          D3DRS_FOGENABLE,     D3DRS_CLIPPLANEENABLE,  D3DRS_SRGBWRITEENABLE,
                                          D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE};
        for (D3DRENDERSTATETYPE st : off)
            m_dev->SetRenderState(st, FALSE);
        m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        m_dev->SetTexture(0, m_heightTex);
        m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        const DWORD filter = m_heightFilter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
        m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
        m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        m_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        m_dev->SetPixelShaderConstantF(0, step, 1);
        m_dev->SetPixelShaderConstantF(1, shade, 1);
        m_dev->SetPixelShaderConstantF(2, known, 1);
        // Clip space, shifted half a texel so texel centres meet pixel centres; uv 0-1 over the grid.
        // uv: the heights' uv (grid convention, cell centres at (i + 0.5) / cells) at the area's corners.
        const float d = 1.0f / kHillCells, gs = 1.0f / (m_spacing * m_cells), go = 0.5f / m_cells;
        const float u0 = (region[0] - region[2]) * gs + go, u1 = (region[0] + region[2]) * gs + go;
        const float v0 = (region[1] - region[2]) * gs + go, v1 = (region[1] + region[2]) * gs + go;
        struct V
        {
            float x, y, z, u, v;
        } q[4] = {{-1 - d, 1 + d, 0, u0, v0}, {1 - d, 1 + d, 0, u1, v0}, {-1 - d, -1 + d, 0, u0, v1},
                  {1 - d, -1 + d, 0, u1, v1}};
        m_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        m_dev->EndScene();
        memcpy(m_hillSun, sun, sizeof(sun));
        m_hillDarkness = darkness;
        m_hillReady = true;
        memcpy(m_hillRegion, region, sizeof(region));
        // Receivers: mask uv = (world - area's low corner) / its width.
        m_hillGrid[0] = 1.0f / (2 * region[2]);
        m_hillGrid[1] = -(region[0] - region[2]) * m_hillGrid[0];
        m_hillGrid[2] = -(region[1] - region[2]) * m_hillGrid[0];
    }
    m_hillState->Apply();
    saved.Restore(m_dev);
    return true;
}

bool TerrainNormals::SaveHillBmp(const std::wstring& path)
{
    return m_hillReady && m_hillSurf && SaveSurfaceBmp(m_dev, m_hillSurf, path);
}

void TerrainNormals::UpdateNormals(int x0, int y0, int x1, int y1)
{
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
        {
            float h = Height(cx, cy);
            DWORD& out = m_normals[(cy & (m_cells - 1)) * m_cells + (cx & (m_cells - 1))];
            if (std::isnan(h))
            {
                out = 0x00808080;  // unknown: alpha 0
                continue;
            }
            // Central differences; a missing neighbour falls back to this cell (a one-sided difference).
            auto at = [&](int x, int y) {
                float v = Height(x, y);
                return std::isnan(v) ? h : v;
            };
            float l = at(cx - 1, cy), r = at(cx + 1, cy), dn = at(cx, cy - 1), up = at(cx, cy + 1);
            float n[3] = {-(r - l) / (2 * m_spacing), -(up - dn) / (2 * m_spacing), 1.0f};
            float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            DWORD c[3];
            for (int k = 0; k < 3; ++k)
                c[k] = static_cast<DWORD>((std::max)(0.0f, (std::min)(255.0f, (n[k] / len * 0.5f + 0.5f) * 255.0f + 0.5f)));
            out = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
        }
    m_normalsNew.Add(x0, y0, x1, y1);
}

IDirect3DTexture9* TerrainNormals::Texture()
{
    if (m_spacing <= 0)
        return nullptr;
    if (!m_tex)
    {
        ULONG before = DeviceRefs(m_dev);
        if (FAILED(m_dev->CreateTexture(m_cells, m_cells, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_tex, nullptr)))
            return nullptr;
        m_internalRefs += DeviceRefs(m_dev) - before;
        m_normalsNew = TerrainCells::Rect::Whole(m_cells);
    }
    // Just the cells new terrain changed (all of them for a new texture: 4 MB at 1024 cells, 16 MB at 2048).
    if (!m_normalsNew.Empty() && Upload<DWORD>(m_tex, m_normalsNew, [&](int x, int y) { return m_normals[Index(x, y)]; }))
        m_normalsNew = TerrainCells::Rect();
    return m_tex;
}

bool TerrainNormals::SaveBmp(const std::wstring& path)
{
    IDirect3DTexture9* tex = Texture();
    if (!tex)
        return false;
    Log::Write("terrain normals: %u buffers read (%u vertices), %u unreadable, spacing %.3f", m_buffers, m_vertices,
               m_unreadable, m_spacing);
    return SaveTextureBmp(tex, path);
}

void TerrainNormals::ReleaseDefault()
{
    m_hillReady = false;
    if (!m_hillTex && !m_hillSurf && !m_hillState)
        return;
    ULONG before = DeviceRefs(m_dev);
    IUnknown* objects[] = {m_hillSurf, m_hillTex, m_hillState};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_hillSurf = nullptr;
    m_hillTex = nullptr;
    m_hillState = nullptr;
    ULONG after = DeviceRefs(m_dev);
    m_internalRefs -= (before > after && m_internalRefs >= before - after) ? before - after : 0;
}

void TerrainNormals::ReleaseAll()
{
    // No reference counting afterwards: this can be the device's last object.
    IUnknown* objects[] = {m_tex, m_hillSurf, m_hillTex, m_hillState, m_heightTex, m_hillVs, m_hillPs, m_shoreTex};
    for (IUnknown* o : objects)
        if (o)
            o->Release();
    m_tex = nullptr;
    m_hillSurf = nullptr;
    m_hillTex = nullptr;
    m_hillState = nullptr;
    m_heightTex = nullptr;
    m_shoreTex = nullptr;  // (each made again, with all of its cells, on use)
    m_hillVs = nullptr;
    m_hillPs = nullptr;
    m_hillReady = false;
    m_internalRefs = 0;
}
