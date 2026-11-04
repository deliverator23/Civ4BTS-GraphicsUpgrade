// Terrain tile groups ([terrain tiles] TileGroups, TileGroup1..8; a group is a "set" here: the INI numbers them from 1,
// everything here is 0-based, so the INI's TileGroup1 is set 0): which heightmap tile variants a map square may use, by
// the terrain of its four corner plots, and variants for hills and peaks.
//
// The engine (Civ4BeyondSword.exe, Steam 3.1.9.0) picks a flat, ocean or coast tile for each square at random from
// its list, with a picker that is deterministic per square (FUN_00477ef0: srand(x * 0x1C81 + y * 0xB5D); rand() %
// count). The same index also picks the coast's wave model (CoastWave<class>_<NN>.nif) and its cell in a 32-cell coast
// atlas, so they stay matched. Hills and peaks take hill1_<mask> / peak_<mask> directly, one tile per corner mask.
// The patch replaces the picker (so a square's index can be kept within its set's share of a list), the two direct
// lookups (so hill1_16..30, 31..45... and the same for peaks are variants, picked the same way), and the coast index's
// two other uses (the atlas cell and the wave model, folded into the art there is, so coast tiles can be any number).
// With one set and stock files the picks are exactly the engine's. Pure logic here; terrain_sets.cpp has the hooks.
#pragma once

#include "terrain_patch.h"  // Site, ApplySites

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace TileSets
{
const int kMaxSets = 8;
const int kMasks = 15;  // hill and peak corner masks 1..15: hill1_1..15 is variant 0, hill1_16..30 variant 1...

// The part of a list of `count` variants a square of set `set` may use: the list split into `sets` equal groups in file
// order. All of it when there aren't enough for every set (or one set).
struct Range
{
    int lo, size;
};
inline Range SetRange(int count, int sets, int set)
{
    const int group = sets > 1 ? count / sets : 0;
    if (group < 1 || set < 0 || set >= sets)
        return {0, count};
    return {set * group, group};
}

// The set most of a square's corner plots belong to (votes: a set, or -1 for a plot that doesn't vote); ties go to the
// lower set; no votes, set 0 (the INI's TileGroup1).
inline int Majority(const int votes[4], int sets)
{
    int counts[kMaxSets] = {};
    for (int k = 0; k < 4; ++k)
        if (votes[k] >= 0 && votes[k] < sets && votes[k] < kMaxSets)
            ++counts[votes[k]];
    int best = 0;
    for (int s = 1; s < sets && s < kMaxSets; ++s)
        if (counts[s] > counts[best])
            best = s;
    return best;
}

// The plots at square (x, y)'s corners, as the engine finds them (FUN_00478260): TL, TR, BR, BL.
inline void Corners(int x, int y, int gridHeight, int out[4][2])
{
    const int top = gridHeight - y;
    const int c[4][2] = {{x - 1, top}, {x, top}, {x, top - 1}, {x - 1, top - 1}};
    memcpy(out, c, sizeof(c));
}

// The engine's own pick: its seed for square (x, y) (already wrapped).
inline unsigned Seed(int x, int y)
{
    return static_cast<unsigned>(x * 0x1C81 + y * 0xB5D);
}

// Coasts: the variant index also picks the wave model (CoastWave<class>_<variant + 1>.nif) and a cell of the water's
// transition texture, a 4 x 8 atlas with 8, 8, 8 and 7 cells for the four classes. The patch folds the index into the
// art there is (cells and models per class; at most the atlas's), so a class's heightmap tiles can be any number.
const int kAtlasCells[4] = {8, 8, 8, 7};
// The coast class of a land-corner mask: one corner 0, two side by side 1, two diagonal 2, three 3; -1 for 0 or 15.
inline int CoastClass(int landMask)
{
    switch (landMask & 15)
    {
    case 1: case 2: case 4: case 8: return 0;
    case 3: case 6: case 9: case 12: return 1;
    case 5: case 10: return 2;
    case 7: case 14: case 13: case 11: return 3;
    default: return -1;
    }
}
// A coast variant index folded into the first `art[class]` (capped by the atlas) cells or models.
inline int CycleIndex(int variant, int landMask, const int art[4])
{
    const int cls = CoastClass(landMask);
    if (cls < 0 || variant < 0)
        return variant;
    const int cap = (std::max)(1, (std::min)(art[cls], kAtlasCells[cls]));
    return variant % cap;
}

// Where the hooks go: Exe::Layout (exe_layout.h; image base 0x400000, which the lookups' list addresses assume). The
// variant picker is replaced by a jump, except from its caller choosing terrain blend textures (blendCaller), which
// always gets the engine's pick. The hill and peak lookups (MOV ECX,[hills.begin]; MOV EDX,[ECX+EAX*4-4] and MOV
// ECX,[peaks.begin]; PUSH ESI; MOV ESI,[ECX+EAX*4-4]) become calls; the lists are std::vector<texture*>s, begin at +4,
// end at +8. The coast class index (FUN_004788e0: ECX x, EAX y, EDX land mask) keeps its own caller, the coast tile
// pick; its calls for the transition atlas's cell (from FUN_004cde80) and the wave model's number (from FUN_006119c0)
// go to thunks.
inline uint32_t HillList(const Exe::Layout& x) { return x.lists[2]; }
inline uint32_t PeakList(const Exe::Layout& x) { return x.lists[3]; }

// [terrain tiles] StandardBlendOnLand1..8: a terrain's blend entries on squares with no water corner. The engine picks
// each terrain layer's blend entry for a square from its art define's TextureBlend<mask> list (FUN_00479920: mask and
// &tile, &rotation, x, y on the stack, popped by RET 0x14; EAX = the terrain; called from the layer records'
// builder, FUN_004795b0, at three sites: Exe::Layout's blendPick and blendPickCalls). A list paired with the coast
// heightmap tiles (painted coasts) would put a coast's cliff on land-to-land borders too; with a land list, those
// squares take an entry from it instead (a square with a water corner, or a corner off the map, keeps the engine's
// pick, so it stays paired).

// The settings. StandardBlendOnLand=TERRAIN_A, TERRAIN_B...: those terrains take BtS's standard blend layout on all-land
// squares (the atlas tile painted for each corner arrangement, unturned: the layout of BtS's own Grass and most
// terrains, so what their TextureBlend lists held before they were paired with the coast tiles).
// StandardBlendOnLand1..8 for an atlas laid out otherwise: "TERRAIN_X: <mask 1>; <mask 2>; ... <mask 14>", each mask
// one or more "tile,rotation" entries separated by spaces (as the XML's TextureBlend lists; rotation 0, 90, 180 or 270);
// these win over the short form.
struct LandBlend
{
    std::string terrain;                         // upper case
    std::vector<std::pair<int, int>> masks[15];  // [1..14]
};
// BtS's standard layout: the tile for corner masks 1..14 (TL 1, TR 2, BR 4, BL 8), unturned.
const int kStandardLandTiles[15] = {0, 8, 1, 2, 6, 13, 10, 3, 5, 9, 7, 4, 12, 11, 14};
inline LandBlend StandardLandBlend(const std::string& terrain)
{
    LandBlend b;
    for (char c : terrain)
        b.terrain += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    for (int mask = 1; mask <= 14; ++mask)
        b.masks[mask].push_back({kStandardLandTiles[mask], 0});
    return b;
}
// The short form's terrain names (separated by commas, semicolons or spaces).
inline std::vector<std::string> LandBlendNames(const std::string& text)
{
    std::vector<std::string> names;
    std::string name;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        const char c = i < text.size() ? text[i] : ' ';
        if (c == ',' || c == ';' || isspace(static_cast<unsigned char>(c)))
        {
            if (!name.empty())
                names.push_back(name);
            name.clear();
        }
        else
            name += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    return names;
}
inline bool ParseLandBlend(const std::string& text, LandBlend& out, std::string& why)
{
    const size_t colon = text.find(':');
    if (colon == std::string::npos)
    {
        why = "no ':' after the terrain";
        return false;
    }
    out = LandBlend();
    for (char c : text.substr(0, colon))
        if (!isspace(static_cast<unsigned char>(c)))
            out.terrain += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    std::string rest = text.substr(colon + 1) + ";";
    int mask = 0;
    size_t start = 0;
    for (size_t semi = rest.find(';'); semi != std::string::npos; start = semi + 1, semi = rest.find(';', start))
    {
        const std::string group = rest.substr(start, semi - start);
        if (group.find_first_not_of(" \t") == std::string::npos && semi + 1 == rest.size())
            break;  // a trailing ';'
        if (++mask > 14)
        {
            why = "more than 14 masks";
            return false;
        }
        size_t at = 0;
        while (true)
        {
            at = group.find_first_not_of(" \t", at);
            if (at == std::string::npos)
                break;
            const size_t end = group.find_first_of(" \t", at);
            const std::string entry = group.substr(at, end == std::string::npos ? std::string::npos : end - at);
            int tile = 0, rotation = -1;
            char tail = 0;
            if (sscanf(entry.c_str(), "%d,%d%c", &tile, &rotation, &tail) != 2 || tile < 1 || tile > 255 ||
                (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270))
            {
                why = "mask " + std::to_string(mask) + ": \"" + entry + "\" isn't tile,rotation";
                return false;
            }
            out.masks[mask].push_back({tile, rotation});
            if (end == std::string::npos)
                break;
            at = end;
        }
        if (out.masks[mask].empty())
        {
            why = "mask " + std::to_string(mask) + " has no entry";
            return false;
        }
    }
    if (mask != 14)
    {
        why = std::to_string(mask) + " masks, not 14";
        return false;
    }
    return !out.terrain.empty() || (why = "no terrain", false);
}

// What the land-blend hook does with each layer's blend pick on a square. The engine builds a square's layers one
// after another in rising LayerOrder (the first, its lowest terrain, drawn whole: mask 15), each picking its blend for
// the mask of its own corners. On a coast square with two or more land terrains, each land layer's mask is only part
// of the land, so their paired coast shapes would be for smaller masks than the coast heightmap tile's. There the
// lowest land layer instead takes its own paired pick for the full land mask (the coast tile's: the same pick the
// engine makes for that list on this square, so it pairs with the tile), and the higher land layers their land
// entries, drawn over it. The hill and peak layers (overlays: their terrain is at none of the corners; their mask is
// the corners that are hill or peak plots) work the same on all-land squares; on a coast square an overlay with a
// paired list that covers only part of the land re-picks for the whole land mask too, painting the whole cliff (Dune
// Wars' Mesa, the peak layer: Iain's choice). Corners: TL, TR, BR, BL; water = water or off the map (as the heightmap
// counts them).
// [terrain tiles] MixedCoastBlend (blendAll): on such a mixed coast square, once the lowest land layer has taken the
// full land mask, every higher land layer with a paired list takes its own paired pick for the full land mask too
// (instead of its land entry), and the square is marked blended: the terrain shader then weights each of those layers
// by its share of the square's land corners (terrain_shadow.hlsl, MixWeight), so the terrains blend across the square
// and agree along every edge with the squares beside it. A square left to the engine stays so. An overlay taking the
// whole land's cliff on a coast (above) marks its square too: its layer is weighted by its share of the land corners
// (peak plots count as the peak layer's in the shader's data, whatever their terrain), so Mesa blends across the square.
struct BlendCorners
{
    int terrain[4];
    bool water[4];
};
enum class BlendAction
{
    Engine,    // the engine's pick stands
    Land,      // the terrain's LandBlend entry for its mask
    FullMask,  // the engine's pick again, for fullMask (the land corners)
};
struct BlendSquare
{
    int x = -0x7fffffff, y = -0x7fffffff;
    bool landSeen = false, untouched = false;
    bool blended = false;  // MixedCoastBlend: this square's paired land layers all took the full land mask
    BlendAction Next(int sx, int sy, const BlendCorners& c, int terrain, int mask, bool hasLand, int& fullMask,
                     bool blendAll = false)
    {
        if (sx != x || sy != y)
        {
            x = sx, y = sy;
            landSeen = untouched = blended = false;
        }
        bool anyWater = false, atLandCorner = false, atAnyCorner = false;
        int full = 0, distinct[4], n = 0;
        for (int k = 0; k < 4; ++k)
        {
            atAnyCorner = atAnyCorner || c.terrain[k] == terrain;
            if (c.water[k])
            {
                anyWater = true;
                continue;
            }
            full |= 1 << k;
            atLandCorner = atLandCorner || c.terrain[k] == terrain;
            bool seen = false;
            for (int i = 0; i < n; ++i)
                seen = seen || distinct[i] == c.terrain[k];
            if (!seen)
                distinct[n++] = c.terrain[k];
        }
        const BlendAction land = hasLand && mask >= 1 && mask <= 14 ? BlendAction::Land : BlendAction::Engine;
        if (!anyWater)
            return land;  // all land: the land entry
        if (!atAnyCorner)  // an overlay (hills, peaks) on a coast: paired, it paints the whole land's cliff
        {
            if (!hasLand || mask < 1 || mask > 14 || mask == full)
                return BlendAction::Engine;  // no paired list, or it covers all the land already: paired as it is
            fullMask = full;
            blended = blended || blendAll;
            return BlendAction::FullMask;
        }
        if (n < 2 || !atLandCorner)
            return BlendAction::Engine;  // one land terrain (paired as it is), or a water layer or hill/peak overlay
        if (!landSeen)
        {
            landSeen = true;
            if (mask == 15 || !hasLand)  // a land terrain drawn whole underneath, or one without a paired list
            {
                untouched = true;
                return BlendAction::Engine;
            }
            fullMask = full;
            blended = blendAll;
            return BlendAction::FullMask;
        }
        if (untouched)
            return BlendAction::Engine;
        if (blendAll && land == BlendAction::Land)  // a higher paired land layer: the whole land too, weighted
        {
            fullMask = full;
            return BlendAction::FullMask;
        }
        return land;
    }
};

inline std::vector<uint8_t> Rel32(uint8_t opcode, uint32_t from, uint32_t to)
{
    const uint32_t rel = to - (from + 5);
    return {opcode, static_cast<uint8_t>(rel), static_cast<uint8_t>(rel >> 8), static_cast<uint8_t>(rel >> 16),
            static_cast<uint8_t>(rel >> 24)};
}

// The three calls to the blend pick, sent to thunk instead.
inline std::vector<TerrainPatch::Site> LandBlendSites(uint32_t base, uint32_t thunk, const Exe::Layout& x)
{
    std::vector<TerrainPatch::Site> sites;
    for (uint32_t rva : x.blendPickCalls)
        sites.push_back({rva, Rel32(0xE8, base + rva, base + x.blendPick), Rel32(0xE8, base + rva, thunk),
                         "terrain layer blend pick (CALL FUN_00479920)"});
    return sites;
}

// The five sites, given where the module is loaded and the thunks' addresses.
inline std::vector<TerrainPatch::Site> Sites(uint32_t base, uint32_t pickerThunk, uint32_t hillThunk, uint32_t peakThunk,
                                             uint32_t atlasThunk, uint32_t waveThunk, const Exe::Layout& x)
{
    std::vector<uint8_t> picker = Rel32(0xE9, base + x.picker, pickerThunk);  // JMP
    std::vector<uint8_t> hills = Rel32(0xE8, base + x.hills, hillThunk);      // CALL; 5 NOPs
    hills.insert(hills.end(), 5, 0x90);
    std::vector<uint8_t> peaks = {0x56};  // PUSH ESI; CALL (sets ESI); 5 NOPs
    std::vector<uint8_t> call = Rel32(0xE8, base + x.peaks + 1, peakThunk);
    peaks.insert(peaks.end(), call.begin(), call.end());
    peaks.insert(peaks.end(), 5, 0x90);
    // MOV ECX,[list.begin] (8B 0D <VA>)
    auto movBegin = [](uint32_t list) {
        const uint32_t at = list + 4;
        return std::vector<uint8_t>{0x8b, 0x0d, static_cast<uint8_t>(at), static_cast<uint8_t>(at >> 8),
                                    static_cast<uint8_t>(at >> 16), static_cast<uint8_t>(at >> 24)};
    };
    std::vector<uint8_t> hillsWas = movBegin(HillList(x)), peaksWas = movBegin(PeakList(x));
    hillsWas.insert(hillsWas.end(), {0x8b, 0x54, 0x81, 0xfc});
    peaksWas.insert(peaksWas.end(), {0x56, 0x8b, 0x74, 0x81, 0xfc});
    return {
        {x.picker, {0x53, 0x55, 0x8b, 0x6c, 0x24}, picker, "variant picker (PUSH EBX; PUSH EBP; MOV EBP,[ESP+10])"},
        {x.hills, hillsWas, hills, "hill tile by mask"},
        {x.peaks, peaksWas, peaks, "peak tile by mask"},
        {x.atlasCall, Rel32(0xE8, base + x.atlasCall, base + x.coastClassFn),
         Rel32(0xE8, base + x.atlasCall, atlasThunk), "coast atlas cell (CALL FUN_004788e0)"},
        {x.waveCall, Rel32(0xE8, base + x.waveCall, base + x.coastClassFn), Rel32(0xE8, base + x.waveCall, waveThunk),
         "coast wave model (CALL FUN_004788e0)"},
    };
}
}  // namespace TileSets
