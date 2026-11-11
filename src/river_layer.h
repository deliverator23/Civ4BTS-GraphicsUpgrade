// Experimental ([terrain tiles] RiverLayerTerrain, off by default): a terrain layer along the map's rivers, on any
// square, so Dune Wars' canyons can have sand floors on all-rock squares, where the engine draws only rock.
//
// The exe's layer builder (FUN_004795b0) writes one 0x30-byte record per square, map squares x 0..GridWidth by y
// 0..GridHeight, row by row, from [landscape + 0x40]: up to six layers, six of each of
//   +0x00 terrain (byte)   +0x06 corner mask (byte, 0 ends the list)   +0x0c blend tile (byte)   +0x12 list index (byte)
//   +0x18 rotation (word)  +0x24 layer order (byte)   +0x2a alpha shader (byte)
// After the terrains it adds TERRAIN_HILL and TERRAIN_PEAK by their corner masks in just this way. The chunks read
// the records (FUN_004ba9e0), batching by terrain, so an added layer draws with its terrain's blend sheet, at its order.
//
// The patch sends the builder's four callers to a function that runs it and then adds a layer to every square a river
// runs through. A river lies on plot edges and a square's middle is a point where plots meet, so a square has up to
// four arms, from its middle to the middle of each side. The arms pick the tile: first tile + arms - 1, unturned, north
// 1, east 2, south 4, west 8. So the layer's terrain needs a blend sheet with 15 tiles there, and any list of its own
// left transparent: Dune Wars uses TERRAIN_HILL, which the mod otherwise draws as nothing (its layer order is 100, over
// every terrain, and it needs no new terrain type, so terrain numbers and saves stay as they are).
//
// [terrain tiles] RiverLayerCaps=<folder>: shape tiles. Then a square's shape picks its blend tile too, and the ground is
// cut: a cap tile is 64 x 64, 8-bit, row 0 north, the most the height grid may be there, as an absolute height (255: no
// cap), so one tile does for rock, mesas and sinks. The engine stamps a square's base tile, hills and peaks in that order
// in all four of its loops, so the patch wraps the peak stamp's four calls and caps the square after it, which also
// covers a plot changing in play. <folder>/cap<arms>_<n>.tga are the shapes, n from 1. A river ends where its vertex
// touches water or a sink, with the coast's cliff still between it and the open ground, so a one-arm square touching
// them takes a mouth, which cuts on through: mouth<arms>_<n>.tga straight on, mouthr and mouthl to the right and left
// corner, looking down the river to its end (MouthKind). All in kOrder: the blend sheet holds them in the same order
// from RiverLayerFirstTile. DuneWars-Messiah's tools/canyon_tiles.py makes both.
//
// Pure logic here (no Windows calls), so the test harness can run it; terrain_sets.cpp has the hook.
#pragma once

#include "exe_layout.h"
#include "terrain_patch.h"  // Site
#include "terrain_sets.h"   // Rel32

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace RiverLayer
{
const int kRecordBytes = 0x30;
const int kLayers = 6;
const uint32_t kRecordsAt = 0x40;  // the records' address in the landscape object

enum Arm
{
    kNorth = 1,
    kEast = 2,
    kSouth = 4,
    kWest = 8
};

// The plots round a square's middle (TileSets::Corners) and whether each has a river: the arms it has. tl/tr/bl are
// whether those corner plots are west of or north of a river; a plot off the map has neither. The river north of the
// middle runs between TL and TR (TL is west of it), south between BL and BR, west between TL and BL (TL is north of it),
// east between TR and BR.
inline int Arms(bool tlWestOf, bool blWestOf, bool tlNorthOf, bool trNorthOf)
{
    return (tlWestOf ? kNorth : 0) | (trNorthOf ? kEast : 0) | (blWestOf ? kSouth : 0) | (tlNorthOf ? kWest : 0);
}

// The blend tile for arms 1..15.
inline int Tile(int arms, int firstTile)
{
    return firstTile + arms - 1;
}

// Adds a whole-square layer to a record, as the builder writes one. False, writing nothing, if its six are taken.
inline bool Add(uint8_t* record, int terrain, int tile, int order)
{
    int i = 0;
    while (i < kLayers && record[6 + i] != 0)
        ++i;
    if (i == kLayers)
        return false;
    const uint16_t rotation = 0;
    record[i] = static_cast<uint8_t>(terrain);
    record[6 + i] = 15;
    record[0xc + i] = static_cast<uint8_t>(tile);
    record[0x12 + i] = 0;
    memcpy(record + 0x18 + i * 2, &rotation, 2);
    record[0x24 + i] = static_cast<uint8_t>(order);
    record[0x2a + i] = 0;
    if (i + 1 < kLayers)  // the end of the list, as the builder marks it
    {
        const uint16_t none = 0xffff;
        ++i;
        record[i] = 0xff;
        record[6 + i] = 0;
        record[0xc + i] = 0xff;
        record[0x12 + i] = 0xff;
        memcpy(record + 0x18 + i * 2, &none, 2);
        record[0x24 + i] = 0xff;
        record[0x2a + i] = 0;
    }
    return true;
}

// Shape tiles.
const int kCapSize = 64;
const int kKinds = 4;  // 0 cap; mouths: 1 straight on, 2 right, 3 left
struct ShapeId
{
    int kind, arms;
};
const ShapeId kOrder[] = {{0, 1},  {0, 2},  {0, 4},  {0, 8},  {0, 3},  {0, 6},  {0, 12}, {0, 9},  {0, 5},
                          {0, 10}, {0, 7},  {0, 14}, {0, 13}, {0, 11}, {0, 15}, {1, 1},  {2, 1},  {3, 1},
                          {1, 2},  {2, 2},  {3, 2},  {1, 4},  {2, 4},  {3, 4},  {1, 8},  {2, 8},  {3, 8}};

struct Shapes
{
    std::vector<std::vector<uint8_t>> tiles;  // kCapSize x kCapSize each, row 0 north, in file order
    int first[kKinds][16] = {};               // the first of each shape's tiles
    int count[kKinds][16] = {};
};

inline std::string CapFile(int kind, int arms, int n)
{
    static const char* const names[kKinds] = {"cap", "mouth", "mouthr", "mouthl"};
    return names[kind] + std::to_string(arms) + "_" + std::to_string(n) + ".tga";
}

// An uncompressed 8-bit grey TGA, kCapSize square, to row 0 north. False if it's anything else.
inline bool ParseCap(const std::vector<uint8_t>& file, std::vector<uint8_t>& out)
{
    const size_t pixels = static_cast<size_t>(kCapSize) * kCapSize;
    if (file.size() < 18 + static_cast<size_t>(file[0]) + pixels || file[1] != 0 || file[2] != 3 || file[16] != 8 ||
        file[12] + file[13] * 256 != kCapSize || file[14] + file[15] * 256 != kCapSize)
        return false;
    const uint8_t* data = file.data() + 18 + file[0];
    const bool topDown = (file[17] & 0x20) != 0;
    out.resize(pixels);
    for (int r = 0; r < kCapSize; ++r)
        memcpy(out.data() + r * kCapSize, data + (topDown ? r : kCapSize - 1 - r) * kCapSize, kCapSize);
    return true;
}

// The engine's deterministic per-square pick (the first rand() after srand(x * 0x1C81 + y * 0xB5D)).
inline int Pick(int x, int y, int count)
{
    uint32_t seed = static_cast<uint32_t>(x) * 0x1C81u + static_cast<uint32_t>(y) * 0xB5Du;
    seed = seed * 214013u + 2531011u;
    return count > 0 ? static_cast<int>((seed >> 16) & 0x7FFF) % count : 0;
}

// Which mouth a one-arm square takes, or 0: `open` is the corner plots that are water or sinks (TL 1, TR 2, BR 4, BL 8).
// The corners either side of the arm are rock, as a river only runs between rock, so it's the far two that matter.
inline int MouthKind(int arms, int open)
{
    int right = 0, left = 0;
    switch (arms)
    {
    case 1: right = 8, left = 4; break;  // the river comes south: west is its right
    case 2: right = 1, left = 8; break;  // west: north
    case 4: right = 2, left = 1; break;  // north: east
    case 8: right = 4, left = 2; break;  // east: south
    default: return 0;
    }
    if (open == 0)
        return 0;
    const bool r = (open & right) != 0, l = (open & left) != 0;
    return r == l ? 1 : r ? 2 : 3;
}

// The tile for a square, an index into Shapes::tiles, or -1 for none. Wrapping maps have the same square at both ends: x
// comes in wrapped.
inline int ShapeTile(const Shapes& s, int arms, int open, int x, int y)
{
    if (arms <= 0 || arms > 15)
        return -1;
    int kind = MouthKind(arms, open);
    if (s.count[kind][arms] == 0)
        kind = 0;
    return s.count[kind][arms] > 0 ? s.first[kind][arms] + Pick(x, y, s.count[kind][arms]) : -1;
}

// height = min(height, cap) over a square of the height grid.
inline void ApplyCap(uint8_t* grid, int stride, const uint8_t* cap)
{
    for (int r = 0; r < kCapSize; ++r, grid += stride, cap += kCapSize)
        for (int c = 0; c < kCapSize; ++c)
            if (grid[c] > cap[c])
                grid[c] = cap[c];
}

// [terrain tiles] RiverLayerDepths=<folder>, in place of RiverLayerCaps: the same tiles and names, but each holds how
// far to lower the ground (0: not at all), so a crack is as deep on a mesa as on the plain. It never takes the ground
// below the sand's level, nor raises ground already lower.
const int kSandLevel = 48;

inline void ApplyDepth(uint8_t* grid, int stride, const uint8_t* depth)
{
    for (int r = 0; r < kCapSize; ++r, grid += stride, depth += kCapSize)
        for (int c = 0; c < kCapSize; ++c)
        {
            const int h = grid[c], lowest = h < kSandLevel ? h : kSandLevel, cut = h - depth[c];
            grid[c] = static_cast<uint8_t>(cut < lowest ? lowest : cut);
        }
}

// How far the canyons lowered the ground, so what drapes itself over the terrain (the culture borders, through the height
// texture) can lie on the ground as it was and bridge a canyon rather than drop into it: canyons follow plot edges,
// where borders run. Kept per cut square (kCapSize x kCapSize levels, the grid's own samples), looked up by world
// position: tile pixel (i, j) of square (x, y) is at plot (x - 1 + i / kCapSize, height - y - j / kCapSize), and plot
// (px, py)'s centre at (x0 + px * plot, y0 + py * plot).
class Cuts
{
public:
    void Map(int width, int height, bool wrapX, float x0, float y0, float plot)
    {
        m_w = width, m_h = height, m_wrapX = wrapX, m_x0 = x0, m_y0 = y0, m_plot = plot;
    }
    bool Mapped() const { return m_plot > 0; }
    // A square after its cut: before and after are its samples (after through the grid's stride). Nothing kept if
    // nothing was lowered.
    void Set(int sx, int sy, const uint8_t* before, const uint8_t* after, int stride)
    {
        std::vector<uint8_t> cut(static_cast<size_t>(kCapSize) * kCapSize + 1);  // the last: the square's typical cut
        std::vector<uint8_t> lowered;
        for (int r = 0; r < kCapSize; ++r)
            for (int c = 0; c < kCapSize; ++c)
            {
                const int d = before[r * kCapSize + c] - after[r * stride + c];
                cut[r * kCapSize + c] = static_cast<uint8_t>(d > 0 ? d : 0);
                if (d > 0)
                    lowered.push_back(static_cast<uint8_t>(d));
            }
        const bool any = !lowered.empty();
        if (any)
        {
            // three quarters of the lowered samples are cut no deeper than this: a trench reads as a trench even with a
            // high spot in the square (a Rugged rise, a mesa's edge), a gorge through a mesa as the gorge it mostly is
            const size_t at = (lowered.size() - 1) * 3 / 4;
            std::nth_element(lowered.begin(), lowered.begin() + at, lowered.end());
            cut.back() = lowered[at];
        }
        if (any)
            m_squares[Key(sx, sy)] = std::move(cut);
        else
            Clear(sx, sy);
        ++m_version;
    }
    void Clear(int sx, int sy)
    {
        if (m_squares.erase(Key(sx, sy)))
            ++m_version;
    }
    // Levels lowered at a world position (the nearest sample; 0 off the map or where nothing was cut).
    float At(float wx, float wy) const
    {
        int at = 0;
        const std::vector<uint8_t>* square = Find(wx, wy, at);
        return square ? static_cast<float>((*square)[at]) : 0.0f;
    }
    // What to add back for a border to lie on: the whole cut in a square whose typical cut is no deeper than kBridgeFull
    // (a trench in rock, 89-128 down to the sand's 48), none where it's kBridgeNone or deeper (a gorge through a mesa,
    // where a lid at the old height looked like a sheet floating over it; the border follows the gorge down, as it does
    // every mesa's cliffs), in between in proportion. By the square's typical cut (three quarters of its lowered samples
    // no deeper; its deepest let one high spot leave a trench unbridged, Iain's trace 2026-11-10 23:03), so a square
    // bridges all of a piece or none of it.
    static constexpr float kBridgeFull = 70, kBridgeNone = 100;
    float Bridge(float wx, float wy) const
    {
        int at = 0;
        const std::vector<uint8_t>* square = Find(wx, wy, at);
        if (!square)
            return 0;
        float w = (kBridgeNone - square->back()) / (kBridgeNone - kBridgeFull);
        w = w < 0 ? 0 : w > 1 ? 1 : w;
        return (*square)[at] * w;
    }
    unsigned Version() const { return m_version; }
    size_t Squares() const { return m_squares.size(); }
    // Debug, for traces: how the cuts are placed, and each cut square as [x, y, typical cut, deepest cut].
    std::string Describe() const
    {
        char head[200];
        snprintf(head, sizeof(head), "\"map\":[%d,%d,%d],\"x0\":%.2f,\"y0\":%.2f,\"plot\":%.2f,\"squares\":[", m_w, m_h,
                 m_wrapX ? 1 : 0, m_x0, m_y0, m_plot);
        std::string out = head;
        bool first = true;
        for (const auto& kv : m_squares)
        {
            const auto& cut = kv.second;
            const uint8_t deepest = *std::max_element(cut.begin(), cut.end() - 1);
            char one[48];
            snprintf(one, sizeof(one), "%s[%d,%d,%d,%d]", first ? "" : ",", kv.first % (m_w + 1), kv.first / (m_w + 1),
                     cut.back(), deepest);
            out += one;
            first = false;
        }
        return out + "]";
    }

private:
    const std::vector<uint8_t>* Find(float wx, float wy, int& at) const
    {
        if (m_squares.empty() || m_plot <= 0)
            return nullptr;
        float px = (wx - m_x0) / m_plot, q = m_h - (wy - m_y0) / m_plot;
        if (m_wrapX && m_w > 0)
            px -= std::floor(px / m_w) * m_w;
        int sx = static_cast<int>(std::floor(px)) + 1, sy = static_cast<int>(std::floor(q));
        int i = static_cast<int>(std::floor((px - std::floor(px)) * kCapSize + 0.5f));
        int j = static_cast<int>(std::floor((q - std::floor(q)) * kCapSize + 0.5f));
        if (i == kCapSize)
            i = 0, ++sx;
        if (j == kCapSize)
            j = 0, ++sy;
        if (sx < 0 || sy < 0 || sx > m_w || sy > m_h)
            return nullptr;
        const auto it = m_squares.find(Key(sx, sy));
        if (it == m_squares.end())
            return nullptr;
        at = j * kCapSize + i;
        return &it->second;
    }
    int Key(int sx, int sy) const { return sy * (m_w + 1) + sx; }
    std::unordered_map<int, std::vector<uint8_t>> m_squares;
    int m_w = 0, m_h = 0;
    bool m_wrapX = false;
    float m_x0 = 0, m_y0 = 0, m_plot = 0;
    unsigned m_version = 0;
};

// The peak stamp's four calls, sent to `thunk`.
inline std::vector<TerrainPatch::Site> CapSites(uint32_t base, uint32_t thunk, const Exe::Layout& x)
{
    std::vector<TerrainPatch::Site> sites;
    for (uint32_t rva : x.peakStampCalls)
        sites.push_back({rva, TileSets::Rel32(0xE8, base + rva, base + x.peakStamp),
                         TileSets::Rel32(0xE8, base + rva, thunk), "terrain peak stamp (CALL FUN_00478e10)"});
    return sites;
}

// The builder's four callers, sent to `thunk`, a __stdcall function of the landscape as the builder is.
inline std::vector<TerrainPatch::Site> Sites(uint32_t base, uint32_t thunk, const Exe::Layout& x)
{
    std::vector<TerrainPatch::Site> sites;
    for (uint32_t rva : x.layerBuilderCalls)
        sites.push_back({rva, TileSets::Rel32(0xE8, base + rva, base + x.layerBuilder),
                         TileSets::Rel32(0xE8, base + rva, thunk), "terrain layer records (CALL FUN_004795b0)"});
    return sites;
}
}  // namespace RiverLayer
