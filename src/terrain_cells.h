// The terrain grid's bookkeeping for partial updates (TerrainNormals): rectangles of cells that changed, split where
// they cross the grid's wrap, and the shore's distance to land worked out over just the cells it can change. Pure
// logic (no Direct3D), so the test harness can check it against working everything out again.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace TerrainCells
{
// A rectangle of cells in grid coordinates (world / spacing, before wrapping), inclusive; empty when x1 < x0.
struct Rect
{
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    bool Empty() const { return x1 < x0 || y1 < y0; }
    void Add(int ax0, int ay0, int ax1, int ay1)
    {
        x0 = (std::min)(x0, ax0), y0 = (std::min)(y0, ay0), x1 = (std::max)(x1, ax1), y1 = (std::max)(y1, ay1);
    }
    void Add(const Rect& r)
    {
        if (!r.Empty())
            Add(r.x0, r.y0, r.x1, r.y1);
    }
    static Rect Whole(int cells) { return {0, 0, cells - 1, cells - 1}; }
};

// A rectangle at most one grid across: one wider (or taller) than the grid keeps the grid's width from its own x0 (y0).
inline Rect ClampToGrid(Rect r, int cells)
{
    if (!r.Empty() && r.x1 - r.x0 + 1 > cells)
        r.x1 = r.x0 + cells - 1;
    if (!r.Empty() && r.y1 - r.y0 + 1 > cells)
        r.y1 = r.y0 + cells - 1;
    return r;
}

// Calls piece(gx0, gy0, gx1, gy1, tx0, ty0) for each part of r (ClampToGrid) that doesn't cross the wrap of a cells x
// cells grid (cells a power of two): grid cells gx0..gx1, gy0..gy1 (inclusive) at texels tx0.., ty0... Each texel once.
template <class Piece>
void ForEachPiece(Rect r, int cells, Piece piece)
{
    if (r.Empty())
        return;
    const int mask = cells - 1;
    r = ClampToGrid(r, cells);
    for (int ys = r.y0; ys <= r.y1;)
    {
        const int ye = (std::min)(r.y1, ys + (mask - (ys & mask)));  // the last row before the wrap
        for (int xs = r.x0; xs <= r.x1;)
        {
            const int xe = (std::min)(r.x1, xs + (mask - (xs & mask)));
            piece(xs, ys, xe, ye, xs & mask, ys & mask);
            xs = xe + 1;
        }
        ys = ye + 1;
    }
}

// The shore: distance to the nearest land (height at or above the water) in cells, as a two-pass chamfer (1 straight,
// 1.414 diagonally), kept up to kShoreKeep cells: past that the texture's byte is full (x 8, 255).
const int kShoreKeep = 32;

// Which cells to work the shore out again for when the heights of `changed` are new: written, the changed cells and
// kShoreKeep around them (new land changes no kept distance farther away), at most one grid across; worked out over
// kShoreKeep more, which holds every land cell those can be nearer than kShoreKeep to (a chamfer path stays in the box
// of its two ends). The window may be wider than the grid: the heights wrap, so that's the map unrolled.
inline void ShoreRects(const Rect& changed, int cells, Rect& write, Rect& window)
{
    write = ClampToGrid({changed.x0 - kShoreKeep, changed.y0 - kShoreKeep, changed.x1 + kShoreKeep,
                         changed.y1 + kShoreKeep}, cells);
    window = {write.x0 - kShoreKeep, write.y0 - kShoreKeep, write.x1 + kShoreKeep, write.y1 + kShoreKeep};
}

// The chamfer distances over window (row by row), from height(x, y) (NaN: unknown, not land).
template <class Height>
std::vector<float> ShoreDistances(const Rect& window, float waterHeight, Height height)
{
    const int w = window.x1 - window.x0 + 1, h = window.y1 - window.y0 + 1;
    const float kFar = 1e9f, kDiagonal = 1.41421356f;
    std::vector<float> dist(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float v = height(window.x0 + x, window.y0 + y);
            dist[static_cast<size_t>(y) * w + x] = !std::isnan(v) && v >= waterHeight ? 0.0f : kFar;
        }
    auto at = [&](int x, int y) -> float& { return dist[static_cast<size_t>(y) * w + x]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float& d = at(x, y);
            if (x > 0)
                d = (std::min)(d, at(x - 1, y) + 1);
            if (y > 0)
            {
                d = (std::min)(d, at(x, y - 1) + 1);
                if (x > 0)
                    d = (std::min)(d, at(x - 1, y - 1) + kDiagonal);
                if (x < w - 1)
                    d = (std::min)(d, at(x + 1, y - 1) + kDiagonal);
            }
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x)
        {
            float& d = at(x, y);
            if (x < w - 1)
                d = (std::min)(d, at(x + 1, y) + 1);
            if (y < h - 1)
            {
                d = (std::min)(d, at(x, y + 1) + 1);
                if (x < w - 1)
                    d = (std::min)(d, at(x + 1, y + 1) + kDiagonal);
                if (x > 0)
                    d = (std::min)(d, at(x - 1, y + 1) + kDiagonal);
            }
        }
    return dist;
}

// A shore texel: r = distance x 8, g = depth below the water x 8 (both full at 32), a = 255; 0 where unknown.
inline uint32_t ShoreTexel(float distance, float height, float waterHeight)
{
    if (std::isnan(height))
        return 0;
    auto byte = [](float v) { return static_cast<uint32_t>((std::min)(255.0f, (std::max)(0.0f, v * 8.0f + 0.5f))); };
    return 0xFF000000u | byte(distance) << 16 | byte(waterHeight - height) << 8;
}
}  // namespace TerrainCells
