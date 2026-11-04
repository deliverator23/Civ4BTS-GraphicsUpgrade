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
// - Colours ([terrain tiles] PlotStampsNTextures): each piece can have a painting beside it (<piece>_colour.dds: square,
//   power of two, full mips, DXT5 or A8R8G8B8; the same two plots as the heightmap, row 0 north; straight alpha, 0 well
//   inside its edge). The terrain shader draws them on the stamped terrain's own layer (terrain_shadow.hlsl, StampColour),
//   so the lightmap, shadows and fog of war apply: it can't choose among separate textures per pixel, so the paintings
//   are copied into the cells of one atlas here, and which cell a plot has goes to it per plot, from the same pick as the
//   heights.
//
// Pure logic here (no Windows calls), so the test harness can run it; terrain_sets.cpp has the hook.
#pragma once

#include "jigsaw.h"  // Piece, AddTile; hill_field.h's Hash, Mix

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace PlotStamps
{
// A painting's DDS file, as the atlas needs it; false, with why, if it won't do.
struct ColourFile
{
    int size = 0;       // texels across
    bool dxt5 = false;  // else A8R8G8B8
    const uint8_t* levels = nullptr;  // the top level; the others follow it
};
const int kMinCell = 4;  // the atlas's coarsest level has cells this size (a DXT block)

inline int LevelsFor(int size)  // from size down to kMinCell
{
    int n = 1;
    while (size > kMinCell)
        size /= 2, ++n;
    return n;
}

inline size_t LevelBytes(int size, bool dxt5)
{
    return dxt5 ? static_cast<size_t>((size + 3) / 4) * ((size + 3) / 4) * 16 : static_cast<size_t>(size) * size * 4;
}

inline bool ParseColourDds(const std::vector<uint8_t>& d, ColourFile& out, std::string& why)
{
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        memcpy(&v, d.data() + at, 4);
        return v;
    };
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0)
        return why = "not a DDS file", false;
    const int h = static_cast<int>(u32(12)), w = static_cast<int>(u32(16)), mips = static_cast<int>((std::max)(u32(28), 1u));
    if (w != h || w < 16 || w > 2048 || (w & (w - 1)))
        return why = std::to_string(w) + "x" + std::to_string(h) + ", not square, a power of two, 16 to 2048", false;
    const uint32_t flags = u32(80), four = u32(84), bits = u32(88), rmask = u32(92), gmask = u32(96), bmask = u32(100),
                   amask = u32(104);
    const bool dxt5 = (flags & 4) && four == 0x35545844u;
    const bool argb = !(flags & 4) && (flags & 0x40) && bits == 32 && rmask == 0xff0000 && gmask == 0xff00 &&
                      bmask == 0xff && amask == 0xff000000u;
    if (!dxt5 && !argb)
        return why = "not DXT5 or A8R8G8B8", false;
    if (mips < LevelsFor(w))
        return why = std::to_string(mips) + " mip levels, needs " + std::to_string(LevelsFor(w)), false;
    size_t need = 128;
    for (int size = w, k = 0; k < LevelsFor(w); ++k, size /= 2)
        need += LevelBytes(size, dxt5);
    if (need > d.size())
        return why = "cut off", false;
    out.size = w;
    out.dxt5 = dxt5;
    out.levels = d.data() + 128;
    return true;
}

// The atlas: whole cells, powers of two across and down.
struct AtlasLayout
{
    int cols = 0, rows = 0, cell = 0, levels = 0;
    int Width() const { return cols * cell; }
    int Height() const { return rows * cell; }
};
inline AtlasLayout LayoutAtlas(int cells, int cellSize)
{
    AtlasLayout a;
    a.cell = cellSize;
    a.levels = LevelsFor(cellSize);
    a.cols = 1;
    while (a.cols * a.cols < cells)
        a.cols *= 2;
    a.rows = 1;
    while (a.rows * a.cols < cells)
        a.rows *= 2;
    return a;
}

// Level `level` of a painting into its cell `cell` of the atlas's same level (`dest`, `pitch` bytes a row of texels, or
// of blocks in DXT5).
inline void CopyCell(const ColourFile& f, const AtlasLayout& a, int cell, int level, uint8_t* dest, int pitch)
{
    const uint8_t* from = f.levels;
    for (int k = 0, size = f.size; k < level; ++k, size /= 2)
        from += LevelBytes(size, f.dxt5);
    const int size = f.size >> level;
    const int perRow = f.dxt5 ? size / 4 : size, unit = f.dxt5 ? 16 : 4;
    const int col = cell % a.cols, row = cell / a.cols;
    for (int r = 0; r < perRow; ++r)
        memcpy(dest + static_cast<size_t>(row * perRow + r) * pitch + static_cast<size_t>(col) * perRow * unit,
               from + static_cast<size_t>(r) * perRow * unit, static_cast<size_t>(perRow) * unit);
}

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
