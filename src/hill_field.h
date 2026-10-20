// Experimental ([terrain tiles] HillField, off by default): hills with coherent multi-plot shapes.
//
// BtS stamps one heightmap tile per map square (the square between four plot centres), chosen by which of its four
// corner plots are hills: hill1_<mask>, the same tile wherever that mask occurs. So a run of hills is a row of the
// same few bumps, and no hill can be wider than a plot. Here the hills of the whole map are one height field instead,
// made of stamps (free-form hill pieces, each made for one, two or three plots on a hex lattice, like Civ 5's
// terrain pieces), and the hill lookup hands the engine that field cut into the square's tile. Neighbouring tiles
// then continue each other exactly, and a three-plot ridge stays one ridge across the squares it covers.
//
// - Fitting (Field::Build): stamps are fitted to runs of hill plots by similarity transforms only (rotation, mirror,
//   uniform scale 0.85..1.2, translation; least squares from the stamp's centres to the plot centres), never
//   sheared or squashed: the pieces' art is drawn square-pixelled, only their lattice differs. Biggest pieces first;
//   within a size the best fit anywhere on the map first (ties broken by a hash, so a map always gets the same
//   hills); runs that touch only diagonally cost 0.15 more. Hill plots left over get a one-plot stamp, turned at
//   random. Every hill plot belongs to exactly one placement.
// - A stamp is confined to the squares round its own plots (Chebyshev distance under 1, full strength to 0.6),
//   because the engine stamps a hill tile only on a square with a hill corner; stamps combine by max.
// - A square's tile (Field::Tile): pixel (i, j) is the field at plot coordinates (x - 1 + i/N, top - j/N), top =
//   grid height - y: the engine lays tiles N apart, so the next square's pixel 0 is this one's pixel N.
// - Changes (Field::Update): the engine re-stamps only the four squares round a plot whose type changed, so a small
//   change is applied locally (a hill plot gone leaves its placement, a new one gets a one-plot stamp): only those
//   four squares' tiles change. A large change (a new map) is fitted again from scratch.
// - Heights: the stamps' heights are scaled so their average peak is the stock hill tiles' 230, with a soft knee
//   into 230..255; the engine then multiplies by the landscape's HillScale (and the coast scale mask) as it does the
//   stock tiles.
//
// Pure logic here (no Windows calls), so the test harness can run it; terrain_sets.cpp has the hook.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace HillField
{
// A hill stamp: a height over flat ground, made for the plots at its centres.
struct Stamp
{
    std::string name;
    int w = 0, h = 0;            // texels; row 0 is the stamp's north edge, column 0 its west edge
    float texelsPerPlot = 64.0f;  // texels one plot (a Civ 5 hex) is across
    std::vector<float> height;    // w x h, over flat
    std::vector<float> cu, cv;    // its centres in plot units from its south-west corner: u east, v north
    // Made by Prepare: half-size copies (box filtered) for sampling coarser than a texel, and the highest value.
    std::vector<std::vector<float>> mips;
    std::vector<int> mipW, mipH;
    float peak = 0.0f;
    int Size() const { return static_cast<int>(cu.size()); }
};

inline void Prepare(Stamp& s)
{
    s.mips.assign(1, s.height);
    s.mipW.assign(1, s.w);
    s.mipH.assign(1, s.h);
    s.peak = 0.0f;
    for (float v : s.height)
        s.peak = (std::max)(s.peak, v);
    while (s.mipW.back() > 1 && s.mipH.back() > 1 && s.mips.size() < 8)
    {
        const int pw = s.mipW.back(), ph = s.mipH.back(), w = pw / 2, h = ph / 2;
        const std::vector<float>& p = s.mips.back();
        std::vector<float> m(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                m[y * w + x] = 0.25f * (p[(2 * y) * pw + 2 * x] + p[(2 * y) * pw + 2 * x + 1] +
                                        p[(2 * y + 1) * pw + 2 * x] + p[(2 * y + 1) * pw + 2 * x + 1]);
        s.mips.push_back(std::move(m));
        s.mipW.push_back(w);
        s.mipH.push_back(h);
    }
}

// Deterministic hashing (no CRT rand: the same map always gets the same hills, whatever else ran).
inline uint32_t Hash(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7feb352du;
    a ^= a >> 15;
    a *= 0x846ca68bu;
    a ^= a >> 16;
    return a;
}
inline uint32_t Mix(uint32_t h, uint32_t v) { return Hash(h ^ (v + 0x9e3779b9u + (h << 6) + (h >> 2))); }
inline float Unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

// The map as the field needs it: plots x 0..width-1 east, y 0..height-1 north (CvMap's), hills[y * width + x].
struct Map
{
    int width = 0, height = 0;
    bool wrapX = false, wrapY = false;
    std::vector<uint8_t> hills;
};

// A plot's index after the map's wraps, or -1 off the map.
inline int PlotIndex(const Map& m, int x, int y)
{
    if (m.wrapX && m.width > 0)
        x = ((x % m.width) + m.width) % m.width;
    if (m.wrapY && m.height > 0)
        y = ((y % m.height) + m.height) % m.height;
    if (x < 0 || y < 0 || x >= m.width || y >= m.height)
        return -1;
    return y * m.width + x;
}

// One stamp put down: plot coordinates in its own frame (where its plots are, unwrapped) -> stamp plot units.
struct Placement
{
    int stamp = -1;
    float inv[4] = {1, 0, 0, 1};  // (u, v) = inv x ((X, Y) - t)
    float tx = 0, ty = 0;
    float scale = 1.0f;
    float cost = 0.0f;
    std::vector<std::array<int, 2>> frame;  // its plots, in its frame
    std::vector<int> plots;                 // the same, as map indices
};

const float kStockPeak = 230.0f;  // the stock hill tiles' height at a hill corner (hill1_1..15)
const float kDiagonalCost = 0.15f;
const float kScaleLo = 0.85f, kScaleHi = 1.2f;

// The least-squares similarity (scale held to kScaleLo..kScaleHi) taking src (mirrored in u if asked) to dst; its
// matrix L (row-major), translation and rms error, in plot units.
inline float Similarity(const float* su, const float* sv, const float* du, const float* dv, int n, bool mirror,
                        float L[4], float t[2])
{
    float mu = 0, mv = 0, nu = 0, nv = 0;
    for (int k = 0; k < n; ++k)
    {
        mu += (mirror ? -su[k] : su[k]) / n;
        mv += sv[k] / n;
        nu += du[k] / n;
        nv += dv[k] / n;
    }
    // z = sum conj(A) B / sum |A|^2 in complex numbers: the rotation and scale that fit best
    float re = 0, im = 0, var = 0;
    for (int k = 0; k < n; ++k)
    {
        const float au = (mirror ? -su[k] : su[k]) - mu, av = sv[k] - mv, bu = du[k] - nu, bv = dv[k] - nv;
        re += au * bu + av * bv;
        im += au * bv - av * bu;
        var += au * au + av * av;
    }
    float s = 1.0f, c = 1.0f, sn = 0.0f;
    if (var > 1e-12f)
    {
        const float mag = std::sqrt(re * re + im * im);
        s = (std::min)(kScaleHi, (std::max)(kScaleLo, mag / var));
        if (mag > 1e-12f)
            c = re / mag, sn = im / mag;
    }
    const float m = mirror ? -1.0f : 1.0f;  // L = s R diag(m, 1)
    L[0] = s * c * m;
    L[1] = -s * sn;
    L[2] = s * sn * m;
    L[3] = s * c;
    t[0] = nu - (s * c * mu - s * sn * mv);
    t[1] = nv - (s * sn * mu + s * c * mv);
    float err = 0;
    for (int k = 0; k < n; ++k)
    {
        const float x = L[0] * su[k] + L[1] * sv[k] + t[0] - du[k], y = L[2] * su[k] + L[3] * sv[k] + t[1] - dv[k];
        err += (x * x + y * y) / n;
    }
    return std::sqrt(err);
}

class Field
{
public:
    void SetStamps(std::vector<Stamp> stamps)
    {
        stamps_ = std::move(stamps);
        float sum = 0;
        int n = 0;
        for (Stamp& s : stamps_)
        {
            Prepare(s);
            if (s.peak > 0)
                sum += s.peak, ++n;
        }
        gain_ = n ? kStockPeak / (sum / n) : 1.0f;
        built_ = false;
    }
    const std::vector<Stamp>& Stamps() const { return stamps_; }
    bool Usable() const
    {
        for (const Stamp& s : stamps_)
            if (s.Size() == 1 && s.w > 1 && s.h > 1)
                return true;  // left-over hill plots need a one-plot stamp
        return false;
    }
    bool Built() const { return built_; }
    const Map& MapNow() const { return map_; }
    const std::vector<Placement>& Placements() const { return placements_; }
    float Gain() const { return gain_; }

    // Fits stamps to every hill of the map, from scratch.
    void Build(const Map& map, uint32_t seed = 0x48494C4Cu)
    {
        map_ = map;
        seed_ = Mix(Mix(seed, static_cast<uint32_t>(map.width)), static_cast<uint32_t>(map.height));
        placements_.clear();
        owner_.assign(map.hills.size(), -1);
        built_ = true;
        if (!Usable())
            return;
        for (int size = 3; size >= 2; --size)
            PlaceGroups(size);
        for (int i = 0; i < static_cast<int>(map_.hills.size()); ++i)
            if (map_.hills[i] && owner_[i] < 0)
                PlaceSingle(i % map_.width, i / map_.width);
    }

    // The map read again: 0 nothing changed, 1 changed plots applied locally, 2 fitted again from scratch.
    int Update(const Map& map, uint32_t seed = 0x48494C4Cu)
    {
        if (!built_ || map.width != map_.width || map.height != map_.height || map.wrapX != map_.wrapX ||
            map.wrapY != map_.wrapY || map.hills.size() != map_.hills.size())
        {
            Build(map, seed);
            return 2;
        }
        std::vector<int> changed;
        int hills = 0;
        for (size_t i = 0; i < map.hills.size(); ++i)
        {
            hills += map.hills[i] ? 1 : 0;
            if ((map.hills[i] != 0) != (map_.hills[i] != 0))
                changed.push_back(static_cast<int>(i));
        }
        if (changed.empty())
            return 0;
        if (static_cast<int>(changed.size()) > (std::max)(8, hills / 4))
        {
            Build(map, seed);
            return 2;
        }
        map_.hills = map.hills;
        for (int i : changed)
        {
            if (!map_.hills[i])
            {  // no longer a hill: out of its placement (which then fades out round it, as its other plots keep it)
                const int o = owner_[i];
                owner_[i] = -1;
                if (o < 0)
                    continue;
                Placement& p = placements_[o];
                for (size_t k = 0; k < p.plots.size(); ++k)
                    if (p.plots[k] == i)
                    {
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

    // The hill corners of engine square (x, y) by the field's map: TL 1, TR 2, BR 4, BL 8 (the engine's hill mask).
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

    // Engine square (x, y)'s n x n tile (row 0 north, column 0 west) into out. False when no placement reaches it.
    bool Tile(int x, int y, int n, uint8_t* out) const
    {
        struct Use
        {
            int placement;
            float ox, oy;  // the square's frame offset for that placement
        };
        Use uses[4];
        int count = 0;
        int corners[4][2];
        Corners(x, y, corners);
        for (int k = 0; k < 4; ++k)
        {
            const int i = PlotIndex(map_, corners[k][0], corners[k][1]);
            const int o = i >= 0 ? owner_[i] : -1;
            if (o < 0)
                continue;
            const Placement& p = placements_[o];
            for (size_t f = 0; f < p.plots.size(); ++f)
                if (p.plots[f] == i)
                {
                    const float ox = static_cast<float>(corners[k][0] - p.frame[f][0]),
                                oy = static_cast<float>(corners[k][1] - p.frame[f][1]);
                    bool seen = false;
                    for (int u = 0; u < count; ++u)
                        seen = seen || (uses[u].placement == o && uses[u].ox == ox && uses[u].oy == oy);
                    if (!seen)
                        uses[count++] = {o, ox, oy};
                }
        }
        if (!count || n < 1)
            return false;
        const float x0 = static_cast<float>(x - 1), top = static_cast<float>(map_.height - y);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const float X = x0 + static_cast<float>(i) / n, Y = top - static_cast<float>(j) / n;
                float v = 0;
                for (int u = 0; u < count; ++u)
                    v = (std::max)(v, Eval(placements_[uses[u].placement], X - uses[u].ox, Y - uses[u].oy, n));
                out[j * n + i] = Byte(v);
            }
        return true;
    }

    // The field at plot coordinates (X, Y), in tile bytes, sampled for n samples a plot (all placements: for tests).
    uint8_t At(float X, float Y, int n) const
    {
        float v = 0;
        for (const Placement& p : placements_)
            for (int ox = -1; ox <= 1; ++ox)
                for (int oy = -1; oy <= 1; ++oy)
                {
                    if ((ox && !map_.wrapX) || (oy && !map_.wrapY))
                        continue;
                    v = (std::max)(v, Eval(p, X + ox * map_.width, Y + oy * map_.height, n));
                }
        return Byte(v);
    }

    uint8_t Byte(float v) const
    {
        float b = v * gain_;
        if (b > kStockPeak)  // a soft knee: the highest peaks keep some shape below the byte's 255
            b = kStockPeak + (255.0f - kStockPeak) * (1.0f - std::exp(-(b - kStockPeak) / (255.0f - kStockPeak)));
        return static_cast<uint8_t>((std::min)(255.0f, (std::max)(0.0f, b + 0.5f)));
    }

    // The corner plots of engine square (x, y), unwrapped: TL, TR, BR, BL (as TileSets::Corners).
    void Corners(int x, int y, int out[4][2]) const
    {
        const int top = map_.height - y;
        const int c[4][2] = {{x - 1, top}, {x, top}, {x, top - 1}, {x - 1, top - 1}};
        memcpy(out, c, sizeof(c));
    }

private:
    std::vector<Stamp> stamps_;
    std::vector<Placement> placements_;
    std::vector<int> owner_;  // by plot index: its placement, or -1
    Map map_;
    uint32_t seed_ = 0;
    float gain_ = 1.0f;
    bool built_ = false;

    bool FreeHill(int i) const { return i >= 0 && map_.hills[i] && owner_[i] < 0; }

    // A placement's height at (X, Y) in its own frame (stamp units, before the gain), confined to its plots.
    float Eval(const Placement& p, float X, float Y, int n) const
    {
        float dist = 2.0f;
        for (const auto& f : p.frame)
            dist = (std::min)(dist, (std::max)(std::fabs(X - f[0]), std::fabs(Y - f[1])));
        if (dist >= 1.0f)
            return 0.0f;
        const Stamp& s = stamps_[p.stamp];
        const float dx = X - p.tx, dy = Y - p.ty;
        const float u = p.inv[0] * dx + p.inv[1] * dy, v = p.inv[2] * dx + p.inv[3] * dy;
        if (u < 0 || v < 0 || u > s.w / s.texelsPerPlot || v > s.h / s.texelsPerPlot)
            return 0.0f;
        // the mip with about a texel a sample (never coarser)
        const float perSample = s.texelsPerPlot / (n * p.scale);
        int level = 0;
        while (level + 1 < static_cast<int>(s.mips.size()) && static_cast<float>(2 << level) <= perSample)
            ++level;
        const float k = static_cast<float>(1 << level);
        const int w = s.mipW[level], h = s.mipH[level];
        float col = u * s.texelsPerPlot / k - 0.5f, row = (s.h - v * s.texelsPerPlot) / k - 0.5f;
        col = (std::min)((std::max)(col, 0.0f), w - 1.001f);
        row = (std::min)((std::max)(row, 0.0f), h - 1.001f);
        const int c0 = static_cast<int>(col), r0 = static_cast<int>(row);
        const int c1 = (std::min)(c0 + 1, w - 1), r1 = (std::min)(r0 + 1, h - 1);
        const float fc = col - c0, fr = row - r0;
        const std::vector<float>& m = s.mips[level];
        const float val = (m[r0 * w + c0] * (1 - fc) + m[r0 * w + c1] * fc) * (1 - fr) +
                          (m[r1 * w + c0] * (1 - fc) + m[r1 * w + c1] * fc) * fr;
        float wt = (std::min)(1.0f, (std::max)(0.0f, (1.0f - dist) / 0.4f));
        wt = wt * wt * (3.0f - 2.0f * wt);
        return val * wt;
    }

    void Add(Placement p)
    {
        const int index = static_cast<int>(placements_.size());
        for (int i : p.plots)
            owner_[i] = index;
        placements_.push_back(std::move(p));
    }

    void PlaceSingle(int x, int y)
    {
        std::vector<int> singles;
        for (int k = 0; k < static_cast<int>(stamps_.size()); ++k)
            if (stamps_[k].Size() == 1)
                singles.push_back(k);
        if (singles.empty())
            return;
        const uint32_t h = Mix(Mix(seed_ ^ 0x51u, static_cast<uint32_t>(x)), static_cast<uint32_t>(y));
        Placement p;
        p.stamp = singles[Hash(h) % singles.size()];
        const Stamp& s = stamps_[p.stamp];
        const float a = 6.2831853f * Unit(Hash(h ^ 0xA11u)), m = (Hash(h ^ 0xB22u) & 1) ? -1.0f : 1.0f;
        const float L[4] = {std::cos(a) * m, -std::sin(a), std::sin(a) * m, std::cos(a)};
        p.tx = x - (L[0] * s.cu[0] + L[1] * s.cv[0]);
        p.ty = y - (L[2] * s.cu[0] + L[3] * s.cv[0]);
        Invert(L, p.inv);
        p.frame.push_back({x, y});
        p.plots.push_back(PlotIndex(map_, x, y));
        Add(std::move(p));
    }

    static void Invert(const float L[4], float out[4])
    {
        const float det = L[0] * L[3] - L[1] * L[2];
        const float d = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
        out[0] = L[3] * d;
        out[1] = -L[1] * d;
        out[2] = -L[2] * d;
        out[3] = L[0] * d;
    }

    // Every connected (8-neighbour) group of `size` hill plots, each once, in frame coordinates (the first plot's own).
    struct Group
    {
        std::array<int, 3> key;  // map indices, sorted
        std::array<std::array<int, 2>, 3> frame;
        int size;
    };
    std::vector<Group> Groups(int size) const
    {
        std::vector<Group> out;
        const int nb[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (int y = 0; y < map_.height; ++y)
            for (int x = 0; x < map_.width; ++x)
            {
                const int ip = y * map_.width + x;
                if (!map_.hills[ip])
                    continue;
                for (const auto& d : nb)
                {
                    const int ax = x + d[0], ay = y + d[1], ia = PlotIndex(map_, ax, ay);
                    if (ia < 0 || ia == ip || !map_.hills[ia])
                        continue;
                    if (size == 2)
                    {
                        Group g{{(std::min)(ip, ia), (std::max)(ip, ia), -1}, {{{x, y}, {ax, ay}, {0, 0}}}, 2};
                        if (ip < ia)  // each pair once, found from its lower plot
                            out.push_back(g);
                        continue;
                    }
                    for (int from = 0; from < 2; ++from)
                        for (const auto& e : nb)
                        {
                            const int bx = (from ? ax : x) + e[0], by = (from ? ay : y) + e[1];
                            const int ib = PlotIndex(map_, bx, by);
                            if (ib < 0 || ib == ip || ib == ia || !map_.hills[ib])
                                continue;
                            std::array<int, 3> key = {ip, ia, ib};
                            std::sort(key.begin(), key.end());
                            if (key[0] != ip)  // each triple found from its lowest plot only
                                continue;
                            out.push_back({key, {{{x, y}, {ax, ay}, {bx, by}}}, 3});
                        }
                }
            }
        std::sort(out.begin(), out.end(), [](const Group& a, const Group& b) { return a.key < b.key; });
        out.erase(std::unique(out.begin(), out.end(), [](const Group& a, const Group& b) { return a.key == b.key; }),
                  out.end());
        return out;
    }

    static int Diagonals(const Group& g)
    {
        int n = 0;
        for (int a = 0; a < g.size; ++a)
        {
            bool side = false;
            for (int b = 0; b < g.size; ++b)
                side = side || (b != a && std::abs(g.frame[a][0] - g.frame[b][0]) +
                                                  std::abs(g.frame[a][1] - g.frame[b][1]) == 1);
            n += side ? 0 : 1;
        }
        return n;
    }

    // The best stamp for each group of `size` plots, then the groups best first, each put down if its plots are free.
    void PlaceGroups(int size)
    {
        struct Fit
        {
            float score;
            Placement p;
            std::array<int, 3> key;
        };
        std::vector<Fit> fits;
        static const int perms3[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        static const int perms2[2][3] = {{0, 1, 0}, {1, 0, 0}};
        for (const Group& g : Groups(size))
        {
            const float extra = kDiagonalCost * Diagonals(g);
            float du[3], dv[3];
            for (int k = 0; k < size; ++k)
                du[k] = static_cast<float>(g.frame[k][0]), dv[k] = static_cast<float>(g.frame[k][1]);
            uint32_t gh = seed_;
            for (int k = 0; k < size; ++k)
                gh = Mix(gh, static_cast<uint32_t>(g.key[k]));
            Fit best{1e9f, {}, g.key};
            for (int si = 0; si < static_cast<int>(stamps_.size()); ++si)
            {
                const Stamp& s = stamps_[si];
                if (s.Size() != size)
                    continue;
                const int (*perms)[3] = size == 3 ? perms3 : perms2;
                for (int pi = 0; pi < (size == 3 ? 6 : 2); ++pi)
                    for (int mirror = 0; mirror < 2; ++mirror)
                    {
                        float su[3], sv[3], L[4], t[2];
                        for (int k = 0; k < size; ++k)
                            su[k] = s.cu[perms[pi][k]], sv[k] = s.cv[perms[pi][k]];
                        const float cost = Similarity(su, sv, du, dv, size, mirror != 0, L, t) + extra;
                        if (cost > 0.3f + kDiagonalCost * size)
                            continue;
                        // near-equal fits: which one wins is the map's hash, so pieces vary
                        const float score =
                            cost + 0.04f * Unit(Mix(Mix(Mix(gh, static_cast<uint32_t>(si)), pi), mirror));
                        if (score >= best.score)
                            continue;
                        Placement p;
                        p.stamp = si;
                        p.tx = t[0];
                        p.ty = t[1];
                        p.scale = std::sqrt(std::fabs(L[0] * L[3] - L[1] * L[2]));
                        p.cost = cost;
                        Invert(L, p.inv);
                        for (int k = 0; k < size; ++k)
                        {
                            p.frame.push_back(g.frame[k]);
                            p.plots.push_back(PlotIndex(map_, g.frame[k][0], g.frame[k][1]));
                        }
                        best = {score, std::move(p), g.key};
                    }
            }
            if (best.p.stamp >= 0)
                fits.push_back(std::move(best));
        }
        std::sort(fits.begin(), fits.end(), [](const Fit& a, const Fit& b) {
            return a.score != b.score ? a.score < b.score : a.key < b.key;
        });
        for (Fit& f : fits)
        {
            bool free = true;
            for (int i : f.p.plots)
                free = free && FreeHill(i);
            if (free)
                Add(std::move(f.p));
        }
    }
};

// Made-up hill stamps (smooth bumps and ridges with some noise, on Civ 5's hex lattice: a plot across, rows a plot
// apart, odd rows half a plot east), for the tests and for trying the field without other stamps. Heights up to about
// 60, as Civ 5's pieces (over its flat 60).
inline float ValueNoise(float x, float y, uint32_t seed)
{
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    auto at = [&](int a, int b) { return Unit(Mix(Mix(seed, static_cast<uint32_t>(a)), static_cast<uint32_t>(b))); };
    const float top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
    const float bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
    return top + (bottom - top) * ty;
}

inline std::vector<Stamp> SyntheticStamps(int texelsPerPlot = 64)
{
    struct Shape
    {
        const char* name;
        int n;
        float c[3][2];
    };
    const Shape shapes[] = {
        {"single_a", 1, {{0, 0}}},          {"single_b", 1, {{0, 0}}},          {"single_c", 1, {{0, 0}}},
        {"pair_row", 2, {{0, 0}, {1, 0}}},  {"pair_up", 2, {{0, 0}, {0.5f, 1}}},
        {"line", 3, {{0, 0}, {1, 0}, {2, 0}}}, {"triangle", 3, {{0, 0}, {1, 0}, {0.5f, 1}}},
        {"bent", 3, {{0, 0}, {1, 0}, {1.5f, 1}}},
    };
    std::vector<Stamp> out;
    uint32_t seed = 0x5EEDu;
    for (const Shape& sh : shapes)
    {
        seed = Hash(seed + 1);
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (int k = 0; k < sh.n; ++k)
            for (int a = 0; a < 2; ++a)
                lo[a] = (std::min)(lo[a], sh.c[k][a]), hi[a] = (std::max)(hi[a], sh.c[k][a]);
        Stamp s;
        s.name = std::string("synthetic_") + sh.name;
        s.texelsPerPlot = static_cast<float>(texelsPerPlot);
        s.w = static_cast<int>(std::ceil((hi[0] - lo[0] + 2) * texelsPerPlot));
        s.h = static_cast<int>(std::ceil((hi[1] - lo[1] + 2) * texelsPerPlot));
        float amp[3], ax[3], ay[3];
        for (int k = 0; k < sh.n; ++k)
        {
            s.cu.push_back(sh.c[k][0] - lo[0] + 1);
            s.cv.push_back(sh.c[k][1] - lo[1] + 1);
            amp[k] = 40 + 20 * Unit(Mix(seed, 10 + k));
            ax[k] = 0.85f + 0.3f * Unit(Mix(seed, 20 + k));
            ay[k] = 0.85f + 0.3f * Unit(Mix(seed, 30 + k));
        }
        s.height.assign(static_cast<size_t>(s.w) * s.h, 0.0f);
        for (int r = 0; r < s.h; ++r)
            for (int c = 0; c < s.w; ++c)
            {
                const float u = (c + 0.5f) / texelsPerPlot, v = (s.h - r - 0.5f) / texelsPerPlot;
                float best = 0, nearest = 1e9f;
                for (int k = 0; k < sh.n; ++k)
                {
                    const float du = (u - s.cu[k]) / ax[k], dv = (v - s.cv[k]) / ay[k];
                    const float d = std::sqrt(du * du + dv * dv);
                    best = (std::max)(best, amp[k] * std::exp(-(d / 0.5f) * (d / 0.5f)));
                    nearest = (std::min)(nearest, std::sqrt((u - s.cu[k]) * (u - s.cu[k]) + (v - s.cv[k]) * (v - s.cv[k])));
                    for (int l = k + 1; l < sh.n; ++l)
                    {  // a ridge between neighbouring centres
                        const float ex = s.cu[l] - s.cu[k], ey = s.cv[l] - s.cv[k], len2 = ex * ex + ey * ey;
                        if (len2 > 1.5f)
                            continue;
                        const float t = (std::min)(1.0f, (std::max)(0.0f, ((u - s.cu[k]) * ex + (v - s.cv[k]) * ey) / len2));
                        const float qx = u - s.cu[k] - t * ex, qy = v - s.cv[k] - t * ey;
                        const float d2 = (qx * qx + qy * qy) / (0.36f * 0.36f);
                        best = (std::max)(best, 0.9f * (std::min)(amp[k], amp[l]) * std::exp(-d2));
                    }
                }
                const float detail = 1 + 0.35f * (ValueNoise(u * 3, v * 3, seed) - 0.5f) +
                                     0.15f * (ValueNoise(u * 8, v * 8, seed ^ 0x77u) - 0.5f);
                float fade = (std::min)(1.0f, (std::max)(0.0f, (0.95f - nearest) / 0.45f));
                fade = fade * fade * (3 - 2 * fade);
                s.height[static_cast<size_t>(r) * s.w + c] = best * detail * fade;
            }
        out.push_back(std::move(s));
    }
    return out;
}

// The tile the hook hands the engine, laid out as the hill stamp (FUN_00478cd0) reads the art's: the texture's +0x38
// is its pixel data, whose +0x50 is the pixels, +0x54 / +0x58 / +0x5c point at the top level's width / height /
// offset, +0x64 the bytes a pixel. Read at once by the stamp (FUN_00479050), so one tile serves every call.
// 32-bit only, as the game.
struct EnginePixels
{
    uint8_t unused[0x50];
    uint32_t pixels, widths, heights, offsets, unused60, bytesPerPixel;
};
struct EngineTexture
{
    uint8_t unused[0x38];
    uint32_t pixelData;
};
static_assert(offsetof(EnginePixels, pixels) == 0x50 && offsetof(EnginePixels, bytesPerPixel) == 0x64,
              "the engine's pixel data layout");
static_assert(offsetof(EngineTexture, pixelData) == 0x38, "the engine's texture layout");

struct EngineTile
{
    EngineTexture texture = {};
    EnginePixels data = {};
    uint32_t width = 0, height = 0, offset = 0;
    std::vector<uint8_t> pixels;

    uint8_t* Pixels(int n)
    {
        pixels.assign(static_cast<size_t>(n) * n, 0);
        width = height = static_cast<uint32_t>(n);
        offset = 0;
        data.pixels = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pixels.data()));
        data.widths = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&width));
        data.heights = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&height));
        data.offsets = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&offset));
        data.bytesPerPixel = 1;
        texture.pixelData = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&data));
        return pixels.data();
    }
    void* Texture() { return &texture; }
};

// A stock tile's top level, read the way the hill stamp reads it: false if there's none.
inline bool TileFormat(const void* texture, int& w, int& h, int& bytesPerPixel)
{
    if (!texture)
        return false;
    uint32_t data;
    memcpy(&data, static_cast<const uint8_t*>(texture) + 0x38, 4);
    if (!data)
        return false;
    const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(data));
    uint32_t widths, heights, bpp;
    memcpy(&widths, p + 0x54, 4);
    memcpy(&heights, p + 0x58, 4);
    memcpy(&bpp, p + 0x64, 4);
    if (!widths || !heights)
        return false;
    w = *reinterpret_cast<const int*>(static_cast<uintptr_t>(widths));
    h = *reinterpret_cast<const int*>(static_cast<uintptr_t>(heights));
    bytesPerPixel = static_cast<int>(bpp);
    return true;
}
}  // namespace HillField
