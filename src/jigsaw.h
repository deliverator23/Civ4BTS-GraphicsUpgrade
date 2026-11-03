// Experimental ([terrain tiles] JigsawPieces1..8, off by default): jigsaw tile sets. A set's plots (flat land or water
// of the terrains it lists) are covered with jigsaw pieces, heightmaps made for one or more plots on the square lattice,
// as one map-wide field, and every square touching one of its plots gets that field added onto its base tile.
//
// The engine gives each square one base tile by which of its corners are land (ocean, flat, or a coast turned to fit),
// so relief can't run across plots and a band of plots next to water is a row of cliffs. With a set:
//   - the square's base tile is still the engine's, picked as if the set's corners were water (Floor=water: a floor
//     where the set's plots meet water, e.g. Dune Wars' dunes on its deep desert), land (Floor=land) or as they are
//     (Floor=keep, the default);
//   - plus the set's field cut for the square (bytes added as the engine adds its tiles, rounded, clamped 0..255).
// Coast textures, blend masks, wave models and the water's atlas don't come from that stamp, so they stay the engine's.
//
// - The pieces (a folder under Assets, the running mod's first, or packed in an FPK): pieces.xml lists layouts, each its
//   plots (x east, y north) and the heightmap files made for them (ParsePiecesXml). A heightmap spans one plot beyond
//   its layout's plots on every side, the same number of pixels a plot both ways, and is zero beyond the squares
//   touching its plots; pixel (col, row) lies at (west + col / ppp, north - (row + 1) / ppp), rows north first. DDS:
//   16-bit grey (L16, R16) holds height x 256, 8-bit grey (L8, R8, or the red of RGB) the height itself (DecodeDds).
//   Heights are in levels; the set's height scale (pieces.xml's fHeightScale, the INI's JigsawPiecesNHeight first) makes
//   grid units of them (the engine's land is 64 a plot above its sea floor in BtS's tiles).
// - Placements (Field::Build): exact lattice moves only, a layout's plots onto the set's plots by one of the 8 turns
//   and mirrors and a whole-plot shift. Biggest layouts first; within a size, the ones that leave the fewest of the
//   set's plots without a free neighbour go first (ties by a hash of the map, so a map always gets the same pieces);
//   every plot left gets a one-plot piece. Each plot is in one placement; pieces overlap round their plots and
//   combine by max (Combine: negative heights by min), within a set and between sets.
// - A square needs only the placements owning its four corner plots. Pixel (i, j) of square (x, y) is the field at
//   plot (x - 1 + i/N, top - j/N), top = grid height - y, as hill_field.h.
// - Changes (Field::Update): as the hill field, a few plots changed are applied locally, so only the four squares round
//   each change differ (the engine re-stamps only those); a new map is placed again from scratch.
//
// Pure logic here (no Windows calls), so the test harness can run it; terrain_sets.cpp has the hook.
#pragma once

#include "hill_field.h"     // Map, PlotIndex, Hash, Mix, Unit
#include "terrain_patch.h"  // Site

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Jigsaw
{
using HillField::Map;
using HillField::PlotIndex;

const float kValuePerLevel = 256.0f;  // a piece's pixel values: height in levels x 256

// A jigsaw piece: a heightmap made for `plots`.
struct Piece
{
    std::string name;
    std::vector<std::array<int, 2>> plots;
    float west = -1, north = 1;
    int w = 0, h = 0;
    float ppp = 64;  // pixels a plot
    std::vector<uint16_t> pixels;
    int Size() const { return static_cast<int>(plots.size()); }
};

// A DDS heightmap's top level as values (levels x 256), rows north first. Reads uncompressed grey: L16 / R16
// (DXGI R16_UNORM) as they are, L8 / A8L8 / R8 (R8_UNORM) and the red of 24- or 32-bit RGB as levels, and A16B16G16R16
// (its red). False, with why, for anything else.
inline bool DecodeDds(const uint8_t* d, size_t n, int& w, int& h, std::vector<uint16_t>& out, std::string& why)
{
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        if (at + 4 <= n)
            memcpy(&v, d + at, 4);
        return v;
    };
    if (n < 128 || memcmp(d, "DDS ", 4) != 0)
        return why = "not a DDS file", false;
    h = static_cast<int>(u32(12));
    w = static_cast<int>(u32(16));
    const uint32_t flags = u32(80), four = u32(84), bits = u32(88), rmask = u32(92);
    if (w < 2 || h < 2 || w > 4096 || h > 4096)
        return why = "bad size", false;
    size_t at = 128;
    enum Kind
    {
        kNone, kU16, kU8, kU16First, kMasked
    } kind = kNone;
    int stride = 0;
    const uint32_t kLuminance = 0x20000, kRgb = 0x40;
    if (four == 0x30315844u)  // "DX10"
    {
        at = 148;
        const uint32_t dxgi = u32(128);
        if (dxgi == 56)  // R16_UNORM
            kind = kU16, stride = 2;
        else if (dxgi == 61)  // R8_UNORM
            kind = kU8, stride = 1;
    }
    else if (four == 81 || ((flags & kLuminance) && bits == 16 && rmask == 0xffff))
        kind = kU16, stride = 2;
    else if (four == 50 || ((flags & kLuminance) && bits == 8))
        kind = kU8, stride = 1;
    else if ((flags & kLuminance) && bits == 16 && rmask == 0xff)  // A8L8
        kind = kU8, stride = 2;
    else if (four == 36)  // A16B16G16R16
        kind = kU16First, stride = 8;
    else if (!four && (flags & kRgb) && (bits == 24 || bits == 32) && rmask)
        kind = kMasked, stride = static_cast<int>(bits / 8);
    if (kind == kNone)
        return why = "not an uncompressed grey heightmap (save it as L16, or L8 / 8-bit RGB)", false;
    const size_t need = at + static_cast<size_t>(w) * h * stride;
    if (need > n)
        return why = "cut off", false;
    int shift = 0;
    while (kind == kMasked && !(rmask & (1u << shift)))
        ++shift;
    out.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.size(); ++i)
    {
        const uint8_t* p = d + at + i * stride;
        uint32_t v = 0;
        switch (kind)
        {
        case kU16: case kU16First: v = p[0] | (p[1] << 8); break;
        case kU8: v = p[0] << 8; break;
        default:
        {
            uint32_t px = 0;
            memcpy(&px, p, stride);
            v = ((px & rmask) >> shift) << 8;
        }
        }
        out[i] = static_cast<uint16_t>((std::min)(v, 65535u));
    }
    return true;
}

// pieces.xml: <JigsawPieces> with an optional <fHeightScale> (grid units a level) and <Layout>s, each <Plots> (x,y
// pairs separated by spaces, the first plot anywhere) and one or more <Piece> (a heightmap file in the same folder).
struct PieceFile
{
    std::vector<std::array<int, 2>> plots;
    std::string file;
};
struct PiecesXml
{
    bool hasHeight = false;
    float heightScale = 1.0f;
    std::vector<PieceFile> pieces;
};

inline bool ParsePiecesXml(std::string text, PiecesXml& out, std::string& why)
{
    out = PiecesXml();
    for (size_t c; (c = text.find("<!--")) != std::string::npos;)  // comments out
    {
        const size_t e = text.find("-->", c);
        text.erase(c, e == std::string::npos ? std::string::npos : e + 3 - c);
    }
    auto trim = [](std::string s) {
        const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    // the text of each <tag>...</tag> in [from, to)
    auto each = [&](const std::string& s, const char* tag, size_t from, size_t to) {
        std::vector<std::array<size_t, 2>> spans;
        const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
        for (size_t a = s.find(open, from); a != std::string::npos && a < to; a = s.find(open, a + 1))
        {
            const size_t b = s.find(close, a);
            if (b == std::string::npos || b > to)
                break;
            spans.push_back({a + open.size(), b});
        }
        return spans;
    };
    if (text.find("<JigsawPieces>") == std::string::npos)
        return why = "no <JigsawPieces>", false;
    for (const auto& s : each(text, "fHeightScale", 0, text.size()))
    {
        out.heightScale = static_cast<float>(atof(trim(text.substr(s[0], s[1] - s[0])).c_str()));
        out.hasHeight = true;
    }
    int layout = 0;
    for (const auto& l : each(text, "Layout", 0, text.size()))
    {
        ++layout;
        const auto plots = each(text, "Plots", l[0], l[1]);
        if (plots.size() != 1)
            return why = "layout " + std::to_string(layout) + ": needs one <Plots>", false;
        std::string list = text.substr(plots[0][0], plots[0][1] - plots[0][0]);
        for (char& c : list)
            if (c == ';' || c == '\t' || c == '\r' || c == '\n')
                c = ' ';
        std::vector<std::array<int, 2>> xy;
        for (size_t a = 0; a < list.size();)
        {
            a = list.find_first_not_of(' ', a);
            if (a == std::string::npos)
                break;
            const size_t b = list.find(' ', a);
            const std::string pair = list.substr(a, b == std::string::npos ? std::string::npos : b - a);
            int x = 0, y = 0;
            char tail = 0;
            if (sscanf_s(pair.c_str(), "%d,%d%c", &x, &y, &tail, 1) != 2)
                return why = "layout " + std::to_string(layout) + ": plot \"" + pair + "\" isn't x,y", false;
            for (const auto& q : xy)
                if (q[0] == x && q[1] == y)
                    return why = "layout " + std::to_string(layout) + ": plot " + pair + " twice", false;
            xy.push_back({x, y});
            a = b;
        }
        if (xy.empty() || xy.size() > 16)
            return why = "layout " + std::to_string(layout) + ": 1 to 16 plots", false;
        const auto files = each(text, "Piece", l[0], l[1]);
        if (files.empty())
            return why = "layout " + std::to_string(layout) + ": no <Piece>", false;
        for (const auto& f : files)
            out.pieces.push_back({xy, trim(text.substr(f[0], f[1] - f[0]))});
    }
    if (out.pieces.empty())
        return why = "no <Layout>", false;
    return true;
}

// A piece from a decoded heightmap and its layout's plots: its place from the plots (one plot beyond them on every
// side), its pixels a plot from its size. False, with why, if the size doesn't fit the layout.
inline bool MakePiece(const std::string& name, const std::vector<std::array<int, 2>>& plots, int w, int h,
                      std::vector<uint16_t> pixels, Piece& p, std::string& why)
{
    int lo[2] = {1 << 20, 1 << 20}, hi[2] = {-(1 << 20), -(1 << 20)};
    for (const auto& q : plots)
        for (int a = 0; a < 2; ++a)
            lo[a] = (std::min)(lo[a], q[a]), hi[a] = (std::max)(hi[a], q[a]);
    const int across = hi[0] - lo[0] + 2, down = hi[1] - lo[1] + 2;
    if (plots.empty() || w % across || h % down || w / across != h / down || w / across < 4)
        return why = name + " is " + std::to_string(w) + "x" + std::to_string(h) + ", not " + std::to_string(across) +
                     "x" + std::to_string(down) + " plots of the same size (its layout's plots and one plot round them)",
               false;
    p = Piece();
    p.name = name;
    p.plots = plots;
    p.west = static_cast<float>(lo[0] - 1);
    p.north = static_cast<float>(hi[1] + 1);
    p.w = w;
    p.h = h;
    p.ppp = static_cast<float>(w / across);
    p.pixels = std::move(pixels);
    return true;
}

// Fields combined: the highest of the raised, the lowest of the lowered.
struct Combine
{
    float up = 0, down = 0;
    void Add(float v)
    {
        up = (std::max)(up, v);
        down = (std::min)(down, v);
    }
    float Value() const { return up + down; }
};

// The 8 turns and mirrors of the square lattice: (x, y) -> (m0 x + m1 y, m2 x + m3 y).
const int kSym[8][4] = {{1, 0, 0, 1},  {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0},
                        {-1, 0, 0, 1}, {0, 1, 1, 0},  {1, 0, 0, -1},  {0, -1, -1, 0}};

struct Placement
{
    int piece = -1, sym = 0;
    int tx = 0, ty = 0;                       // map plot = sym x piece plot + t (in the placement's frame)
    std::vector<std::array<int, 2>> frame;    // its plots, in its own frame (unwrapped)
    std::vector<int> plots;                   // the same, as map indices
    std::vector<std::array<int, 2>> removed;  // plots that left the set: faded out round each (its frame)
    std::vector<int> removedPlots;            // the same, as map indices
};

class Field
{
public:
    // unitsPerValue: grid units per pixel value (the height scale / kValuePerLevel).
    void SetPieces(std::vector<Piece> pieces, float unitsPerValue)
    {
        pieces_ = std::move(pieces);
        units_ = unitsPerValue;
        built_ = false;
    }
    const std::vector<Piece>& Pieces() const { return pieces_; }
    bool Usable() const
    {
        for (const Piece& p : pieces_)
            if (p.Size() == 1)
                return true;  // plots left over need a one-plot piece
        return false;
    }
    bool Built() const { return built_; }
    const Map& MapNow() const { return map_; }
    const std::vector<Placement>& Placements() const { return placements_; }

    // map.hills: the set's plots.
    void Build(const Map& map, uint32_t seed = 0x4A494753u)
    {
        map_ = map;
        seed_ = HillField::Mix(HillField::Mix(seed, static_cast<uint32_t>(map.width)), static_cast<uint32_t>(map.height));
        placements_.clear();
        owner_.assign(map.hills.size(), -1);
        ghosts_.assign(map.hills.size(), {});
        built_ = true;
        if (!Usable())
            return;
        int most = 0;
        for (const Piece& p : pieces_)
            most = (std::max)(most, p.Size());
        for (int size = most; size >= 2; --size)
            PlaceSize(size);
        for (int i = 0; i < static_cast<int>(map_.hills.size()); ++i)
            if (map_.hills[i] && owner_[i] < 0)
                PlaceSingle(i % map_.width, i / map_.width);
    }

    // The map read again: 0 nothing changed, 1 changed plots applied locally, 2 placed again from scratch.
    int Update(const Map& map, uint32_t seed = 0x4A494753u)
    {
        if (!built_ || map.width != map_.width || map.height != map_.height || map.wrapX != map_.wrapX ||
            map.wrapY != map_.wrapY || map.hills.size() != map_.hills.size())
        {
            Build(map, seed);
            return 2;
        }
        std::vector<int> changed;
        int in = 0;
        for (size_t i = 0; i < map.hills.size(); ++i)
        {
            in += map.hills[i] ? 1 : 0;
            if ((map.hills[i] != 0) != (map_.hills[i] != 0))
                changed.push_back(static_cast<int>(i));
        }
        if (changed.empty())
            return 0;
        if (static_cast<int>(changed.size()) > (std::max)(8, in / 4))
        {
            Build(map, seed);
            return 2;
        }
        map_.hills = map.hills;
        for (int i : changed)
        {
            if (!map_.hills[i])
            {
                const int o = owner_[i];
                owner_[i] = -1;
                if (o < 0)
                    continue;
                Placement& p = placements_[o];
                for (size_t k = 0; k < p.plots.size(); ++k)
                    if (p.plots[k] == i)
                    {
                        p.removed.push_back(p.frame[k]);
                        p.removedPlots.push_back(i);
                        ghosts_[i].push_back(o);
                        p.plots.erase(p.plots.begin() + k);
                        p.frame.erase(p.frame.begin() + k);
                        break;
                    }
            }
            else if (owner_[i] < 0 && Usable())
                PlaceSingle(i % map_.width, i / map_.width);
        }
        return 1;
    }

    // The set's corners of engine square (x, y): TL 1, TR 2, BR 4, BL 8 (the engine's corner bits).
    int Mask(int x, int y) const
    {
        int corners[4][2];
        Corners(x, y, corners);
        int mask = 0;
        for (int k = 0; k < 4; ++k)
        {
            const int i = PlotIndex(map_, corners[k][0], corners[k][1]);
            if (i >= 0 && map_.hills[i])
                mask |= 1 << k;
        }
        return mask;
    }

    // Engine square (x, y)'s n x n tile in grid units (row 0 north, column 0 west). False when no placement reaches it.
    bool Tile(int x, int y, int n, float* out) const
    {
        struct Use
        {
            int placement, ox, oy;
        };
        std::vector<Use> uses;
        int corners[4][2];
        Corners(x, y, corners);
        auto use = [&](int o, int cx, int cy, const std::array<int, 2>& f) {
            const int ox = cx - f[0], oy = cy - f[1];
            for (const Use& u : uses)
                if (u.placement == o && u.ox == ox && u.oy == oy)
                    return;
            uses.push_back({o, ox, oy});
        };
        for (int k = 0; k < 4; ++k)
        {
            const int i = PlotIndex(map_, corners[k][0], corners[k][1]);
            if (i < 0)
                continue;
            if (owner_[i] >= 0)  // the placement this corner plot is in
            {
                const Placement& p = placements_[owner_[i]];
                for (size_t f = 0; f < p.plots.size(); ++f)
                    if (p.plots[f] == i)
                        use(owner_[i], corners[k][0], corners[k][1], p.frame[f]);
            }
            for (int o : ghosts_[i])  // and those it has left, which fade out over its squares
            {
                const Placement& p = placements_[o];
                for (size_t f = 0; f < p.removedPlots.size(); ++f)
                    if (p.removedPlots[f] == i)
                        use(o, corners[k][0], corners[k][1], p.removed[f]);
            }
        }
        if (uses.empty() || n < 1)
            return false;
        const float x0 = static_cast<float>(x - 1), top = static_cast<float>(map_.height - y);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const float X = x0 + static_cast<float>(i) / n, Y = top - static_cast<float>(j) / n;
                float v = 0;
                for (const Use& u : uses)
                    v = (std::max)(v, Eval(placements_[u.placement], X - u.ox, Y - u.oy));
                out[j * n + i] = v * units_;
            }
        return true;
    }

    // The field at plot coordinates (X, Y) in grid units, from every placement (for tests).
    float At(float X, float Y) const
    {
        float v = 0;
        for (const Placement& p : placements_)
            for (int ox = -1; ox <= 1; ++ox)
                for (int oy = -1; oy <= 1; ++oy)
                {
                    if ((ox && !map_.wrapX) || (oy && !map_.wrapY))
                        continue;
                    v = (std::max)(v, Eval(p, X + ox * map_.width, Y + oy * map_.height));
                }
        return v * units_;
    }

    void Corners(int x, int y, int out[4][2]) const
    {
        const int top = map_.height - y;
        const int c[4][2] = {{x - 1, top}, {x, top}, {x, top - 1}, {x - 1, top - 1}};
        memcpy(out, c, sizeof(c));
    }

private:
    std::vector<Piece> pieces_;
    std::vector<Placement> placements_;
    std::vector<int> owner_;
    std::vector<std::vector<int>> ghosts_;  // by plot index: placements it has left
    Map map_;
    uint32_t seed_ = 0;
    float units_ = 1.0f;
    bool built_ = false;

    static float Smooth(float t)
    {
        t = (std::min)(1.0f, (std::max)(0.0f, t));
        return t * t * (3 - 2 * t);
    }

    // A placement's pixel value at (X, Y) in its own frame.
    float Eval(const Placement& pl, float X, float Y) const
    {
        const Piece& p = pieces_[pl.piece];
        const int* m = kSym[pl.sym];
        const float dx = X - pl.tx, dy = Y - pl.ty;
        // the inverse of an orthogonal matrix is its transpose
        const float u = m[0] * dx + m[2] * dy, v = m[1] * dx + m[3] * dy;
        const float col = (u - p.west) * p.ppp, row = (p.north - v) * p.ppp - 1;
        if (col < -0.001f || row < -0.001f || col > p.w - 0.999f || row > p.h - 0.999f)
            return 0.0f;
        const int c0 = (std::min)(static_cast<int>(col + 0.001f), p.w - 1),
                  r0 = (std::min)(static_cast<int>(row + 0.001f), p.h - 1);
        const float fc = (std::max)(0.0f, col - c0), fr = (std::max)(0.0f, row - r0);
        const int c1 = (std::min)(c0 + 1, p.w - 1), r1 = (std::min)(r0 + 1, p.h - 1);
        const uint16_t* px = p.pixels.data();
        float val = (px[r0 * p.w + c0] * (1 - fc) + px[r0 * p.w + c1] * fc) * (1 - fr) +
                    (px[r1 * p.w + c0] * (1 - fc) + px[r1 * p.w + c1] * fc) * fr;
        if (!pl.removed.empty() && val > 0)
        {
            // faded out over each removed plot's own squares: full where nearer a kept plot than 0.6 or a removed one
            // no nearer than 1, none at a removed plot itself
            float keep = 0;
            for (const auto& f : pl.frame)
                keep = (std::max)(keep, Smooth((1.0f - (std::max)(std::fabs(X - f[0]), std::fabs(Y - f[1]))) / 0.4f));
            float away = 1;
            for (const auto& r : pl.removed)
                away = (std::min)(away, Smooth(((std::max)(std::fabs(X - r[0]), std::fabs(Y - r[1])) - 0.5f) / 0.5f));
            val *= (std::max)(keep, away);
        }
        return val;
    }

    void Add(Placement p)
    {
        const int index = static_cast<int>(placements_.size());
        for (int i : p.plots)
            owner_[i] = index;
        placements_.push_back(std::move(p));
    }

    // One of the pieces of a layout (the pieces whose plots are the same), picked by hash.
    int PickVersion(int piece, uint32_t h) const
    {
        std::vector<int> same;
        for (int k = 0; k < static_cast<int>(pieces_.size()); ++k)
            if (pieces_[k].plots == pieces_[piece].plots)
                same.push_back(k);
        return same[HillField::Hash(h) % same.size()];
    }

    void PlaceSingle(int x, int y)
    {
        int single = -1;
        for (int k = 0; k < static_cast<int>(pieces_.size()) && single < 0; ++k)
            if (pieces_[k].Size() == 1)
                single = k;
        if (single < 0)
            return;
        const uint32_t h = HillField::Mix(HillField::Mix(seed_ ^ 0x51u, static_cast<uint32_t>(x)), static_cast<uint32_t>(y));
        Placement p;
        p.piece = PickVersion(single, h);
        p.sym = static_cast<int>(HillField::Hash(h ^ 0xB22u) % 8);
        const int* m = kSym[p.sym];
        const auto& q = pieces_[p.piece].plots[0];
        p.tx = x - (m[0] * q[0] + m[1] * q[1]);
        p.ty = y - (m[2] * q[0] + m[3] * q[1]);
        p.frame.push_back({x, y});
        p.plots.push_back(PlotIndex(map_, x, y));
        Add(std::move(p));
    }

    bool Free(int i) const { return i >= 0 && map_.hills[i] && owner_[i] < 0; }

    // Every placement of a `size`-plot layout onto free plots of the set; the best first, each put down if still free.
    void PlaceSize(int size)
    {
        struct Cand
        {
            float score;
            std::vector<int> key;  // sorted map indices, then layout and turn
            Placement p;
        };
        std::vector<Cand> cands;
        std::vector<int> layouts;  // the first piece of each distinct layout of this size
        for (int k = 0; k < static_cast<int>(pieces_.size()); ++k)
        {
            if (pieces_[k].Size() != size)
                continue;
            bool seen = false;
            for (int l : layouts)
                seen = seen || pieces_[l].plots == pieces_[k].plots;
            if (!seen)
                layouts.push_back(k);
        }
        const int nb[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (int y = 0; y < map_.height; ++y)
            for (int x = 0; x < map_.width; ++x)
            {
                if (!Free(y * map_.width + x))
                    continue;
                for (int layout : layouts)
                    for (int sym = 0; sym < 8; ++sym)
                    {
                        const int* m = kSym[sym];
                        const auto& plots = pieces_[layout].plots;
                        // (x, y) is the layout's first plot: each placement is found from one plot only
                        const int tx = x - (m[0] * plots[0][0] + m[1] * plots[0][1]);
                        const int ty = y - (m[2] * plots[0][0] + m[3] * plots[0][1]);
                        Placement p;
                        p.sym = sym;
                        p.tx = tx;
                        p.ty = ty;
                        bool ok = true;
                        for (const auto& q : plots)
                        {
                            const int px = m[0] * q[0] + m[1] * q[1] + tx, py = m[2] * q[0] + m[3] * q[1] + ty;
                            const int i = PlotIndex(map_, px, py);
                            if (!Free(i) || std::find(p.plots.begin(), p.plots.end(), i) != p.plots.end())
                            {
                                ok = false;
                                break;
                            }
                            p.frame.push_back({px, py});
                            p.plots.push_back(i);
                        }
                        if (!ok)
                            continue;
                        // plots of the set next to it that it would leave with no other neighbour in the set: worse
                        int orphans = 0;
                        for (const auto& f : p.frame)
                            for (const auto& d : nb)
                            {
                                const int j = PlotIndex(map_, f[0] + d[0], f[1] + d[1]);
                                if (j < 0 || !map_.hills[j] || std::find(p.plots.begin(), p.plots.end(), j) != p.plots.end())
                                    continue;
                                bool other = false;
                                for (const auto& e : nb)
                                {
                                    const int k2 = PlotIndex(map_, j % map_.width + e[0], j / map_.width + e[1]);
                                    other = other || (k2 >= 0 && k2 != j && map_.hills[k2] &&
                                                      std::find(p.plots.begin(), p.plots.end(), k2) == p.plots.end());
                                }
                                orphans += other ? 0 : 1;
                            }
                        Cand c;
                        c.key = p.plots;
                        std::sort(c.key.begin(), c.key.end());
                        c.key.push_back(layout);
                        c.key.push_back(sym);
                        uint32_t h = seed_;
                        for (int v : c.key)
                            h = HillField::Mix(h, static_cast<uint32_t>(v));
                        c.score = orphans + HillField::Unit(h);
                        p.piece = PickVersion(layout, h ^ 0x7E57u);
                        c.p = std::move(p);
                        cands.push_back(std::move(c));
                    }
            }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.score != b.score ? a.score < b.score : a.key < b.key;
        });
        for (Cand& c : cands)
        {
            bool free = true;
            for (int i : c.p.plots)
                free = free && Free(i);
            if (free)
                Add(std::move(c.p));
        }
    }
};

// What a set's plots count as for the base tile.
enum Base
{
    kKeep, kWater, kLand
};

// The land corners the engine's base stamp is given (corner bits TL 1, TR 2, BR 4, BL 8): inSet = corners in a set,
// ownLand = those corners' land as the map was read, toWater / toLand = the corners whose set says so. add: a set's
// tile goes on after. If the engine's mask disagrees with the map as read about a set's corner (the plots aren't as
// read, or the corners are in another order), the square is left to the engine.
inline int BaseMask(int landMask, int inSet, int ownLand, int toWater, int toLand, bool& add)
{
    add = inSet != 0 && ((landMask ^ ownLand) & inSet) == 0;
    return add ? (((landMask & ~toWater) | toLand) & 15) : landMask;
}

// Adds an n x n tile of grid units into the engine's height bytes (rowBytes a row, rows rows) at sample (ox, oy), as
// the engine's stamp adds (rounded, clamped 0..255). False, writing nothing, if it doesn't fit.
inline bool AddTile(uint8_t* cells, int rowBytes, int rows, int ox, int oy, int n, const float* units)
{
    if (!cells || n < 1 || ox < 0 || oy < 0 || ox + n > rowBytes || oy + n > rows)
        return false;
    for (int j = 0; j < n; ++j)
    {
        uint8_t* row = cells + static_cast<size_t>(oy + j) * rowBytes + ox;
        for (int i = 0; i < n; ++i)
        {
            const int v = row[i] + static_cast<int>(std::lround(units[j * n + i]));
            row[i] = static_cast<uint8_t>((std::min)(255, (std::max)(0, v)));
        }
    }
    return true;
}

// The base tile stamp (FUN_00478a90: EDI = the square's land corners, [ESP+4] the height grid, [ESP+8] x, EBX = y;
// RET 8), hooked at its entry: SUB ESP,0x10; CMP EDI,1 (6 bytes) become a JMP to the thunk and a NOP; the thunk runs
// those two itself and comes back at +6. Its address: Exe::Layout's baseStamp.
inline std::vector<TerrainPatch::Site> Sites(uint32_t base, uint32_t thunk, const Exe::Layout& x)
{
    const uint32_t rel = thunk - (base + x.baseStamp + 5);
    return {{x.baseStamp,
             {0x83, 0xec, 0x10, 0x83, 0xff, 0x01},
             {0xe9, static_cast<uint8_t>(rel), static_cast<uint8_t>(rel >> 8), static_cast<uint8_t>(rel >> 16),
              static_cast<uint8_t>(rel >> 24), 0x90},
             "base tile stamp (SUB ESP,0x10; CMP EDI,1)"}};
}

// The engine's height grid object as the stamps read it: +4 the byte grid (+0x50 bytes a row, +0x54 rows, +0x58 the
// bytes), +0x30 / +0x34 samples a plot across and down (17, 33, 65: a tile is one fewer).
struct GridView
{
    uint8_t* cells = nullptr;
    int rowBytes = 0, rows = 0, cellX = 0, cellY = 0;
};
inline bool ReadGrid(const void* grid, GridView& g)
{
    if (!grid)
        return false;
    const auto* o = static_cast<const uint8_t*>(grid);
    uint32_t inner;
    int32_t cx, cy;
    memcpy(&inner, o + 4, 4);
    memcpy(&cx, o + 0x30, 4);
    memcpy(&cy, o + 0x34, 4);
    if (!inner)
        return false;
    const auto* in = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(inner));
    uint32_t cells;
    memcpy(&g.rowBytes, in + 0x50, 4);
    memcpy(&g.rows, in + 0x54, 4);
    memcpy(&cells, in + 0x58, 4);
    g.cells = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(cells));
    g.cellX = cx - 1;
    g.cellY = cy - 1;
    return g.cells && g.rowBytes > 0 && g.rows > 0 && g.cellX > 0 && g.cellX == g.cellY;
}
}  // namespace Jigsaw
