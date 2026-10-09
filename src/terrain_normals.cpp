#include "terrain_normals.h"
#include "overlay.h"

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
    if (!m_done.insert(vb).second || stride < 12)
    {
        vb->Release();
        return;
    }
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
    // mtxWorld: three register rows (float4x3, translation in .w).
    float w[12];
    m_dev->GetVertexShaderConstantF(static_cast<UINT>(vs.world), w, 3);
    const UINT count = (d.Size - offset) / stride;
    const BYTE* base = static_cast<const BYTE*>(data) + offset;
    std::vector<float> world(count * 3);
    for (UINT i = 0; i < count; ++i)
    {
        const float* p = reinterpret_cast<const float*>(base + i * stride);
        for (int r = 0; r < 3; ++r)
            world[i * 3 + r] = w[r * 4] * p[0] + w[r * 4 + 1] * p[1] + w[r * 4 + 2] * p[2] + w[r * 4 + 3];
    }
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
        m_spacing = step;
        m_heights.assign(kCells * kCells, std::numeric_limits<float>::quiet_NaN());
        m_normals.assign(kCells * kCells, 0x00808080);
        Log::Write("terrain normals: first terrain vertex buffer: pool %u, usage 0x%x, fvf 0x%x, stride %u, %u vertices; "
                   "grid spacing %.3f world units (%u cells cover %.0f units, wrapping)",
                   d.Pool, d.Usage, d.FVF, stride, count, m_spacing, kCells, m_spacing * kCells);
    }
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    unsigned offGrid = 0;
    for (UINT i = 0; i < count; ++i)
    {
        float fx = world[i * 3] / m_spacing, fy = world[i * 3 + 1] / m_spacing;
        int cx = static_cast<int>(std::floor(fx + 0.5f)), cy = static_cast<int>(std::floor(fy + 0.5f));
        if (std::fabs(fx - cx) > 0.05f || std::fabs(fy - cy) > 0.05f)
        {
            ++offGrid;  // not on the grid (shouldn't happen for BtS terrain); skipped
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
        // Hill shadows: the new cells, and terrain they shade or are shaded by (up to 64 cells = 1440 units away).
        const int reach = 64;
        if (!m_hillPending)
            m_hillX0 = x0 - reach, m_hillY0 = y0 - reach, m_hillX1 = x1 + reach, m_hillY1 = y1 + reach;
        else
            m_hillX0 = (std::min)(m_hillX0, x0 - reach), m_hillY0 = (std::min)(m_hillY0, y0 - reach),
            m_hillX1 = (std::max)(m_hillX1, x1 + reach), m_hillY1 = (std::max)(m_hillY1, y1 + reach);
        m_hillPending = true;
    }
    vb->Release();
}

float TerrainNormals::HeightAt(float x, float y)
{
    int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float fx = x - ix, fy = y - iy;
    float a = Height(ix, iy), b = Height(ix + 1, iy), c = Height(ix, iy + 1), d = Height(ix + 1, iy + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;  // NaN propagates
}

void TerrainNormals::UpdateHills(int x0, int y0, int x1, int y1)
{
    if (x1 - x0 >= static_cast<int>(kCells))
        x0 = 0, x1 = kCells - 1;
    if (y1 - y0 >= static_cast<int>(kCells))
        y0 = 0, y1 = kCells - 1;
    // Towards the sun, per half-cell step: across the grid (in cells) and up (world units).
    float t[3] = {-m_hillSun[0], -m_hillSun[1], -m_hillSun[2]};
    float hor = std::sqrt(t[0] * t[0] + t[1] * t[1]);
    const bool overhead = hor < 1e-4f || t[2] <= 0;
    const float stepX = overhead ? 0 : t[0] / hor * 0.5f, stepY = overhead ? 0 : t[1] / hor * 0.5f;
    const float rise = overhead ? 0 : t[2] / hor * 0.5f * m_spacing;
    const float soft = (std::max)(0.01f, m_hillSoftness);
    const int maxSteps = overhead ? 0 : (std::min)(256, static_cast<int>((m_maxHeight - m_minHeight) / rise) + 2);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
        {
            float h = Height(cx, cy);
            float shade = 0;
            if (!std::isnan(h))
            {
                float x = static_cast<float>(cx), y = static_cast<float>(cy), z = h, excess = 0;
                for (int s = 0; s < maxSteps; ++s)
                {
                    x += stepX, y += stepY, z += rise;
                    if (z > m_maxHeight)
                        break;
                    float th = HeightAt(x, y);
                    if (!std::isnan(th))
                        excess = (std::max)(excess, th - z);
                }
                shade = (std::min)(1.0f, excess / soft);
            }
            DWORD a = static_cast<DWORD>(shade * 255 + 0.5f);
            DWORD l = static_cast<DWORD>((1 - m_hillDarkness * shade) * 255 + 0.5f);
            m_hill[(cy & (kCells - 1)) * kCells + (cx & (kCells - 1))] = (a << 24) | (l << 16) | (l << 8) | l;
        }
    // Upload just these rows (a band of a gradual refresh), unless they wrap round the grid.
    MarkHillRows(y0, y1);
}

void TerrainNormals::MarkHillRows(int y0, int y1)
{
    if (y0 < 0 || y1 >= static_cast<int>(kCells))
        y0 = 0, y1 = kCells - 1;
    if (!m_hillUpload)
        m_uploadY0 = y0, m_uploadY1 = y1;
    else
        m_uploadY0 = (std::min)(m_uploadY0, y0), m_uploadY1 = (std::max)(m_uploadY1, y1);
    m_hillUpload = true;
}

void TerrainNormals::RelightHills()
{
    for (DWORD& t : m_hill)
    {
        DWORD a = t >> 24;
        DWORD l = static_cast<DWORD>((1 - m_hillDarkness * a / 255.0f) * 255 + 0.5f);
        t = (a << 24) | (l << 16) | (l << 8) | l;
    }
    MarkHillRows(0, kCells - 1);
}

IDirect3DTexture9* TerrainNormals::HillTexture(const float* sunDirection, float darkness)
{
    if (m_spacing <= 0)
        return nullptr;
    float sun[3] = {sunDirection[0], sunDirection[1], sunDirection[2]};
    float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    for (float& v : sun)
        v /= len > 0 ? len : 1;
    if (m_hill.empty())
        m_hill.assign(kCells * kCells, 0xFFFFFFFF & 0x00FFFFFF);  // lit, no shadow
    const bool first = m_hillSoftness < 0;
    if (first || m_hillSoftness != g_config.hillShadowSoftness)
    {
        // First time (or softness changed): everything at once.
        memcpy(m_hillSun, sun, sizeof(sun));
        m_hillDarkness = darkness;
        m_hillSoftness = g_config.hillShadowSoftness;
        UpdateHills(0, 0, kCells - 1, kCells - 1);
        m_hillPending = false;
        m_hillRefreshRow = -1;
    }
    else
    {
        // The sun moved (more than about a quarter of a degree): a gradual refresh, a band of rows per frame, so a
        // moving sun (the light cycle) doesn't stall a frame. A refresh under way restarts with the newest sun.
        float dot = sun[0] * m_hillSun[0] + sun[1] * m_hillSun[1] + sun[2] * m_hillSun[2];
        if (dot < 0.99999f && m_hillRefreshRow < 0)
        {
            memcpy(m_hillSun, sun, sizeof(sun));
            m_hillRefreshRow = 0;
        }
        // Darkness only changes the light factor (rgb) worked out from the shade (alpha): no march needed. Small
        // changes wait, so a slowly changing darkness doesn't upload the mask every frame.
        if (std::fabs(darkness - m_hillDarkness) >= 0.01f)
        {
            m_hillDarkness = darkness;
            RelightHills();
        }
        if (m_hillBudget && m_hillRefreshRow >= 0)
        {
            const int band = 64;
            UpdateHills(0, m_hillRefreshRow, kCells - 1, (std::min)(m_hillRefreshRow + band, static_cast<int>(kCells)) - 1);
            m_hillRefreshRow += band;
            if (m_hillRefreshRow >= static_cast<int>(kCells))
                m_hillRefreshRow = -1;
            m_hillBudget = false;
        }
        if (m_hillPending)
        {
            UpdateHills(m_hillX0, m_hillY0, m_hillX1, m_hillY1);
            m_hillPending = false;
        }
    }
    if (!m_hillTex)
    {
        ULONG before = DeviceRefs(m_dev);
        if (FAILED(m_dev->CreateTexture(kCells, kCells, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_hillTex, nullptr)))
            return nullptr;
        m_internalRefs += DeviceRefs(m_dev) - before;
        MarkHillRows(0, kCells - 1);
    }
    if (m_hillUpload)
    {
        D3DLOCKED_RECT lr;
        const RECT rows = {0, m_uploadY0, static_cast<LONG>(kCells), m_uploadY1 + 1};
        if (SUCCEEDED(m_hillTex->LockRect(0, &lr, &rows, 0)))
        {
            for (int y = m_uploadY0; y <= m_uploadY1; ++y)
                memcpy(static_cast<BYTE*>(lr.pBits) + (y - m_uploadY0) * lr.Pitch, &m_hill[y * kCells], kCells * 4);
            m_hillTex->UnlockRect(0);
        }
        m_hillUpload = false;
    }
    return m_hillTex;
}

bool TerrainNormals::SaveHillBmp(const std::wstring& path)
{
    // As last drawn (or, before any, as the settings say).
    IDirect3DTexture9* tex = m_hillSoftness < 0 ? HillTexture(g_config.sunDirection, g_config.shadowDarkness)
                                                : HillTexture(m_hillSun, m_hillDarkness);
    return tex && SaveTextureBmp(tex, path);
}

void TerrainNormals::UpdateNormals(int x0, int y0, int x1, int y1)
{
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
        {
            float h = Height(cx, cy);
            DWORD& out = m_normals[(cy & (kCells - 1)) * kCells + (cx & (kCells - 1))];
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
    m_dirty = true;
}

IDirect3DTexture9* TerrainNormals::Texture()
{
    if (m_spacing <= 0)
        return nullptr;
    if (!m_tex)
    {
        ULONG before = DeviceRefs(m_dev);
        if (FAILED(m_dev->CreateTexture(kCells, kCells, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_tex, nullptr)))
            return nullptr;
        m_internalRefs += DeviceRefs(m_dev) - before;
        m_dirty = true;
    }
    if (m_dirty)
    {
        // Whole upload (4 MB): only while new terrain comes into view.
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(m_tex->LockRect(0, &lr, nullptr, 0)))
        {
            for (UINT y = 0; y < kCells; ++y)
                memcpy(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch, &m_normals[y * kCells], kCells * 4);
            m_tex->UnlockRect(0);
        }
        m_dirty = false;
    }
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

void TerrainNormals::ReleaseAll()
{
    if (m_tex)
        m_tex->Release();  // no reference counting afterwards: this can be the device's last object
    m_tex = nullptr;
    if (m_hillTex)
        m_hillTex->Release();
    m_hillTex = nullptr;
    m_internalRefs = 0;
}
