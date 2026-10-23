// Experimental ([terrain tiles] PlotStamps1..8, off by default): one stamp per plot. Each plot of the terrains a set
// lists (e.g. Dune Wars' Mountains, single peaks on its Mesa) gets one of the set's one-plot pieces, picked by the plot's
// feature (the Nth feature listed takes the Nth piece, so the stamp matches the model a player without GraphicsUpgrade
// sees; no listed feature: picked by a hash of the plot). Unlike the hill field or the jigsaw pieces, nothing is
// fitted across plots: a stamp stands on its own plot, unturned, and neighbouring stamps combine by max.
//
// - The pieces are jigsaw pieces (jigsaw.h: pieces.xml and its DDS heightmaps, read the same way); only one-plot
//   layouts are used. A one-plot heightmap spans the plot and one plot beyond it on every side, centre to centre: two
//   plots across, its centre the plot's centre.
// - Heights: the pieces' values are levels x 256; the set's height scale (the INI's PlotStampsNHeight, grid units a
//   level; else pieces.xml's fHeightScale, world units a level, over the landscape's fZScale, world units a grid step)
//   makes grid units of them. They're added into the engine's height grid after the square's base tile, as the jigsaw
//   pieces are (terrain_sets.cpp), so they stand on what the engine stamps there (a peak's tile included) and aren't
//   scaled by fPeakScale; only the grid's 255 caps them.
// - A square's tile: the stamps of its four corner plots, sampled as Jigsaw::Field::Tile samples (pixel (i, j) of square
//   (x, y) at plot (x - 1 + i/N, top - j/N), top = grid height - y), so the four squares round a plot continue each
//   other. The engine re-stamps only those four when a plot changes, and each is worked out from the plots as they are
//   then, so there is nothing to keep up to date.
//
// Pure logic here (no Windows calls), so the test harness can run it; terrain_sets.cpp has the hook.
#pragma once

#include "jigsaw.h"  // Piece, AddTile; hill_field.h's Hash, Mix

#include <algorithm>
#include <cstdint>
#include <vector>

namespace PlotStamps
{
// The piece a plot takes: the one at its feature's place in `features` (the INI's PlotStampsNFeatures as feature
// types; more features than pieces wrap round), else one picked by a hash of the plot (x, y wrapped onto the map).
inline int Pick(int feature, const std::vector<int>& features, int pieces, int x, int y)
{
    if (pieces <= 0)
        return -1;
    for (size_t k = 0; k < features.size(); ++k)
        if (feature >= 0 && features[k] == feature)
            return static_cast<int>(k % pieces);
    return static_cast<int>(
        HillField::Hash(HillField::Mix(HillField::Mix(0x57A4Bu, static_cast<uint32_t>(x)), static_cast<uint32_t>(y))) %
        pieces);
}

// A one-plot piece's value (levels x 256) at (u, v) plot widths east and north of its plot's centre: bilinear between
// pixels, 0 outside the heightmap.
inline float Sample(const Jigsaw::Piece& p, float u, float v)
{
    if (p.plots.empty() || p.pixels.empty())
        return 0.0f;
    u += static_cast<float>(p.plots[0][0]);  // the piece's own plot, wherever its layout put it
    v += static_cast<float>(p.plots[0][1]);
    const float col = (u - p.west) * p.ppp, row = (p.north - v) * p.ppp - 1;
    if (col < -0.001f || row < -0.001f || col > p.w - 0.999f || row > p.h - 0.999f)
        return 0.0f;
    const int c0 = (std::min)(static_cast<int>(col + 0.001f), p.w - 1),
              r0 = (std::min)(static_cast<int>(row + 0.001f), p.h - 1);
    const float fc = (std::max)(0.0f, col - c0), fr = (std::max)(0.0f, row - r0);
    const int c1 = (std::min)(c0 + 1, p.w - 1), r1 = (std::min)(r0 + 1, p.h - 1);
    const uint16_t* px = p.pixels.data();
    return (px[r0 * p.w + c0] * (1 - fc) + px[r0 * p.w + c1] * fc) * (1 - fr) +
           (px[r1 * p.w + c0] * (1 - fc) + px[r1 * p.w + c1] * fc) * fr;
}

// A stamp on a square's corner plot: the piece, the plot as the square's corners name it (unwrapped, TileSets::Corners)
// and grid units a pixel value.
struct Stamp
{
    const Jigsaw::Piece* piece = nullptr;
    int x = 0, y = 0;
    float units = 0;
};

// Engine square (x, y)'s n x n tile in grid units (row 0 north, column 0 west), its corners' stamps combined by max.
inline void Tile(const Stamp* stamps, int count, int x, int y, int gridHeight, int n, float* out)
{
    const float x0 = static_cast<float>(x - 1), top = static_cast<float>(gridHeight - y);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const float X = x0 + static_cast<float>(i) / n, Y = top - static_cast<float>(j) / n;
            float v = 0;
            for (int s = 0; s < count; ++s)
                if (stamps[s].piece)
                    v = (std::max)(v, Sample(*stamps[s].piece, X - stamps[s].x, Y - stamps[s].y) * stamps[s].units);
            out[j * n + i] = v;
        }
}

// The samples of an n x n tile at (ox, oy) that adding `total` would push past 255 where `stamp` raises them: the
// headroom the stamps lack (Jigsaw::AddTile clamps them there).
inline int Clipped(const uint8_t* cells, int rowBytes, int rows, int ox, int oy, int n, const float* total,
                   const float* stamp)
{
    if (!cells || n < 1 || ox < 0 || oy < 0 || ox + n > rowBytes || oy + n > rows)
        return 0;
    int clipped = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            if (stamp[j * n + i] > 0.5f &&
                cells[static_cast<size_t>(oy + j) * rowBytes + ox + i] + std::lround(total[j * n + i]) > 255)
                ++clipped;
    return clipped;
}
}  // namespace PlotStamps
