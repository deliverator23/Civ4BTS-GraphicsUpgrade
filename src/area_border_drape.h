// BtS's area borders (the settler's city-site guide: [textures] AreaBorderTextures) laid over the terrain: the geometry,
// with no device, so the tests can check it directly (area_borders.h does the drawing).
//
// BtS draws the guide as fixed-function tiles a plot across (a 4 x 4 atlas of edge and corner pieces), every vertex at
// one height (20.5 in Dune Wars) with the depth test off: a flat sheet under the hills, painted over everything, so it
// shows away from the plots it marks (lower on the screen where the ground is higher). Here each triangle is cut into
// small ones no more than a grid cell across, each vertex is set onto the terrain's heights (the proxy's height grid,
// bilinear, plus a little), then moved towards the camera along its own line of sight. On that line it stays at the
// same place on the screen, only nearer in depth, so drawn with the depth test on, ground that rises between its
// vertices (less than that in front of it) doesn't cover it, while a ridge in front does.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace Drape
{
// The terrain's heights over a small area: cell (cx0 + i, cy0 + j) is at world ((cx0 + i) x spacing, (cy0 + j) x
// spacing), v[j * w + i] its height (NaN where it hasn't been read).
struct Heights
{
    int cx0 = 0, cy0 = 0, w = 0, h = 0;
    float spacing = 0;
    std::vector<float> v;

    // Bilinear between the four cells round (x, y); NaN outside the area or where a cell that counts (one with any
    // weight there) is unknown. A point a hair past the edge (up to a hundredth of a cell) counts as on it: the guide's
    // corners are on grid cells, and a vertex cut onto its edge can round to just outside.
    float At(float x, float y) const
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        if (spacing <= 0 || w < 2 || h < 2 || v.size() < static_cast<size_t>(w) * h)
            return nan;
        float fx = x / spacing - cx0, fy = y / spacing - cy0;
        const float slack = 0.01f;
        if (!(fx >= -slack && fy >= -slack && fx <= w - 1 + slack && fy <= h - 1 + slack))
            return nan;
        fx = (std::min)((std::max)(fx, 0.0f), static_cast<float>(w - 1));
        fy = (std::min)((std::max)(fy, 0.0f), static_cast<float>(h - 1));
        const int ix = (std::min)(static_cast<int>(fx), w - 2), iy = (std::min)(static_cast<int>(fy), h - 2);
        const float tx = fx - ix, ty = fy - iy;
        const float weight[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
        const float cell[4] = {v[iy * w + ix], v[iy * w + ix + 1], v[(iy + 1) * w + ix], v[(iy + 1) * w + ix + 1]};
        float sum = 0;
        for (int k = 0; k < 4; ++k)
            if (weight[k] > 0)
            {
                if (std::isnan(cell[k]))
                    return nan;
                sum += weight[k] * cell[k];
            }
        return sum;
    }
};

// Where the parts of a fixed-function vertex are (its FVF), to interpolate them: every 4 bytes a float, but for the
// colours (D3DCOLOR, interpolated byte by byte). Only untransformed, unblended positions (D3DFVF_XYZ) qualify.
struct Layout
{
    UINT stride = 0;
    UINT normalAt = 0;            // byte offset of the normal (0: none)
    std::vector<uint8_t> colour;  // per 4-byte slot: 1 = a D3DCOLOR
};

inline bool LayoutFor(DWORD fvf, UINT stride, Layout& out)
{
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || stride < 12 || stride % 4 != 0)
        return false;
    out.colour.assign(stride / 4, 0);
    out.normalAt = 0;
    UINT at = 12;
    if (fvf & D3DFVF_NORMAL)
        out.normalAt = at, at += 12;
    if (fvf & D3DFVF_PSIZE)
        at += 4;
    if (fvf & D3DFVF_DIFFUSE)
    {
        if (at + 4 > stride)
            return false;
        out.colour[at / 4] = 1, at += 4;
    }
    if (fvf & D3DFVF_SPECULAR)
    {
        if (at + 4 > stride)
            return false;
        out.colour[at / 4] = 1, at += 4;
    }
    const UINT sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    static const UINT floats[4] = {2, 3, 4, 1};  // D3DFVF_TEXTUREFORMAT2, 3, 4, 1
    for (UINT t = 0; t < sets; ++t)
        at += 4 * floats[(fvf >> (16 + 2 * t)) & 3];
    out.stride = stride;
    return at == stride;
}

// (x, y, z, 1) times a row-vector (Direct3D) matrix.
inline void Transform(const float* p, const float* m, float* out)
{
    for (int c = 0; c < 4; ++c)
        out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
}

// p (world) moved up to pull world units towards the camera along the line through it that keeps its place on the
// screen (towards the eye; with an orthographic camera, along the view), never more than half way to the near plane.
// toClip: world -> clip (row-vector), fromClip its inverse.
inline void Pull(const float* toClip, const float* fromClip, float* p, float pull)
{
    float clip[4];
    Transform(p, toClip, clip);
    if (!(std::fabs(clip[3]) > 1e-6f))
        return;
    const float ndc[3] = {clip[0] / clip[3], clip[1] / clip[3], 0};  // the same place on the screen, at the near plane
    float near4[4];
    Transform(ndc, fromClip, near4);
    if (!(std::fabs(near4[3]) > 1e-12f))
        return;
    const float d[3] = {near4[0] / near4[3] - p[0], near4[1] / near4[3] - p[1], near4[2] / near4[3] - p[2]};
    const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(distance > 1e-3f))
        return;
    const float k = (std::min)(pull, 0.5f * distance) / distance;
    for (int c = 0; c < 3; ++c)
        p[c] += d[c] * k;
}

// A triangle with any area (BtS's buffer for the guide also holds unused tiles, every vertex at 0: left out).
inline bool HasArea(const float* a, const float* b, const float* c)
{
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2], z = e1[0] * e2[1] - e1[1] * e2[0];
    return x * x + y * y + z * z >= 1e-6f;
}

struct Stats
{
    UINT trianglesIn = 0;   // triangles with any area
    UINT trianglesOut = 0;  // after cutting
    UINT vertices = 0;
    UINT cuts = 0;          // the most pieces any edge was cut into
    float lowest = 0, highest = 0;  // the ground under the draped vertices
    bool tooMany = false;   // Build failed for want of 16-bit indices (else, if it failed, for want of ground)
};

// The draw (vertices of layout l in world space, every three of tris a triangle) cut up and draped: each triangle with
// any area cut into n x n smaller ones (n: its longest edge over maxEdge, rounded up, at most 64), each new vertex
// interpolated from the corners (normals and colours too, so the light on it is as before), set onto the ground
// (heights + lift) and pulled
// (toClip, fromClip: the camera, as for Pull). Out: vertices of the same layout and 16-bit indices. False, with
// nothing usable, if a vertex has no ground under it or there would be more than 65535 vertices.
inline bool Build(const Layout& l, const uint8_t* verts, size_t vertexCount, const std::vector<uint32_t>& tris,
                  float maxEdge, const Heights& heights, float lift, const float* toClip, const float* fromClip,
                  float pull, std::vector<uint8_t>& out, std::vector<uint16_t>& outIdx, Stats& st)
{
    out.clear();
    outIdx.clear();
    st = Stats();
    if (l.stride < 12 || maxEdge <= 0)
        return false;
    const UINT slots = l.stride / 4;
    std::vector<uint8_t> v(l.stride);
    bool first = true;
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        const uint8_t* corner[3];
        float pos[3][3];
        bool inRange = true;
        for (int k = 0; k < 3; ++k)
        {
            inRange = inRange && tris[t + k] < vertexCount;
            if (!inRange)
                break;
            corner[k] = verts + static_cast<size_t>(tris[t + k]) * l.stride;
            memcpy(pos[k], corner[k], 12);
        }
        if (!inRange)
            return false;
        if (!HasArea(pos[0], pos[1], pos[2]))
            continue;
        float longest = 0;
        for (int k = 0; k < 3; ++k)
        {
            const float* a = pos[k];
            const float* b = pos[(k + 1) % 3];
            const float dx = b[0] - a[0], dy = b[1] - a[1];
            longest = (std::max)(longest, std::sqrt(dx * dx + dy * dy));  // across the ground
        }
        const int n = (std::max)(1, (std::min)(64, static_cast<int>(std::ceil(longest / maxEdge - 1e-4f))));
        const size_t base = out.size() / l.stride;
        if (base + static_cast<size_t>(n + 1) * (n + 2) / 2 > 65535)
        {
            st.tooMany = true;
            return false;
        }
        st.cuts = (std::max)(st.cuts, static_cast<UINT>(n));
        ++st.trianglesIn;
        // Vertex (i, j), i + j <= n: corner 0 + i/n of the way to corner 1 + j/n to corner 2.
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i + j <= n; ++i)
            {
                const float b = static_cast<float>(i) / n, c = static_cast<float>(j) / n, a = 1 - b - c;
                for (UINT s = 0; s < slots; ++s)
                {
                    if (l.colour[s])
                    {
                        uint8_t px[3][4];
                        for (int k = 0; k < 3; ++k)
                            memcpy(px[k], corner[k] + 4 * s, 4);
                        for (int ch = 0; ch < 4; ++ch)
                        {
                            const float x = a * px[0][ch] + b * px[1][ch] + c * px[2][ch];
                            v[4 * s + ch] = static_cast<uint8_t>((std::min)(255.0f, (std::max)(0.0f, x + 0.5f)));
                        }
                    }
                    else
                    {
                        float f[3];
                        for (int k = 0; k < 3; ++k)
                            memcpy(&f[k], corner[k] + 4 * s, 4);
                        const float x = a * f[0] + b * f[1] + c * f[2];
                        memcpy(&v[4 * s], &x, 4);
                    }
                }
                float p[3];
                memcpy(p, v.data(), 12);
                const float ground = heights.At(p[0], p[1]);
                if (std::isnan(ground))
                    return false;
                st.lowest = first ? ground : (std::min)(st.lowest, ground);
                st.highest = first ? ground : (std::max)(st.highest, ground);
                first = false;
                p[2] = ground + lift;
                Pull(toClip, fromClip, p, pull);
                memcpy(v.data(), p, 12);
                out.insert(out.end(), v.begin(), v.end());
            }
        // Row j starts at index row(j) = sum of (n + 1 - r) for r < j.
        auto at = [&](int i, int j) {
            const int row = j * (n + 1) - j * (j - 1) / 2;
            return static_cast<uint16_t>(base + row + i);
        };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i + j < n; ++i)
            {
                const uint16_t q[3] = {at(i, j), at(i + 1, j), at(i, j + 1)};
                outIdx.insert(outIdx.end(), q, q + 3);
                ++st.trianglesOut;
                if (i + j + 1 < n)
                {
                    const uint16_t r[3] = {at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
                    outIdx.insert(outIdx.end(), r, r + 3);
                    ++st.trianglesOut;
                }
            }
    }
    st.vertices = static_cast<UINT>(out.size() / l.stride);
    return st.trianglesIn > 0;
}
}  // namespace Drape
