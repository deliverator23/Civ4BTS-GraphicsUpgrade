// Terrain tile sets ([terrain tiles] Sets, Set1..Set8; the INI numbers them from 1, everything here is 0-based, so
// the INI's Set1 is set 0): which heightmap tile variants a map square may use, by the terrain
// of its four corner plots, and variants for hills and peaks.
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
// lower set; no votes, set 0 (the INI's Set1).
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

// Where the hooks go (RVAs; Steam build 3.1.9.0, image base 0x400000, which the lookups' list addresses assume):
const uint32_t kPicker = 0x77ef0;    // the variant picker: replaced by a jump
const uint32_t kBlendCaller = 0x79979;  // its caller choosing terrain blend textures: always the engine's pick
const uint32_t kHills = 0x78ce0;     // MOV ECX,[hills.begin]; MOV EDX,[ECX+EAX*4-4]
const uint32_t kPeaks = 0x78e20;     // MOV ECX,[peaks.begin]; PUSH ESI; MOV ESI,[ECX+EAX*4-4]
const uint32_t kHillList = 0xb85360, kPeakList = 0xb85370;  // std::vector<texture*>: begin at +4, end at +8 (VAs)
const uint32_t kSrand = 0xa5a4e0, kRand = 0xa5a4dc;         // the exe's CRT srand / rand (import slots, VAs)
const uint32_t kCoastClassFn = 0x788e0;  // FUN_004788e0: a coast square's variant index (ECX x, EAX y, EDX land mask)
const uint32_t kAtlasCall = 0xcdec2;     // its call for the transition atlas's cell (FUN_004cde80)
const uint32_t kWaveCall = 0x211a18;     // its call for the wave model's number (FUN_006119c0)

inline std::vector<uint8_t> Rel32(uint8_t opcode, uint32_t from, uint32_t to)
{
    const uint32_t rel = to - (from + 5);
    return {opcode, static_cast<uint8_t>(rel), static_cast<uint8_t>(rel >> 8), static_cast<uint8_t>(rel >> 16),
            static_cast<uint8_t>(rel >> 24)};
}

// The five sites, given where the module is loaded and the thunks' addresses.
inline std::vector<TerrainPatch::Site> Sites(uint32_t base, uint32_t pickerThunk, uint32_t hillThunk, uint32_t peakThunk,
                                             uint32_t atlasThunk, uint32_t waveThunk)
{
    std::vector<uint8_t> picker = Rel32(0xE9, base + kPicker, pickerThunk);  // JMP
    std::vector<uint8_t> hills = Rel32(0xE8, base + kHills, hillThunk);      // CALL; 5 NOPs
    hills.insert(hills.end(), 5, 0x90);
    std::vector<uint8_t> peaks = {0x56};  // PUSH ESI; CALL (sets ESI); 5 NOPs
    std::vector<uint8_t> call = Rel32(0xE8, base + kPeaks + 1, peakThunk);
    peaks.insert(peaks.end(), call.begin(), call.end());
    peaks.insert(peaks.end(), 5, 0x90);
    return {
        {kPicker, {0x53, 0x55, 0x8b, 0x6c, 0x24}, picker, "variant picker (PUSH EBX; PUSH EBP; MOV EBP,[ESP+10])"},
        {kHills, {0x8b, 0x0d, 0x64, 0x53, 0xb8, 0x00, 0x8b, 0x54, 0x81, 0xfc}, hills, "hill tile by mask"},
        {kPeaks, {0x8b, 0x0d, 0x74, 0x53, 0xb8, 0x00, 0x56, 0x8b, 0x74, 0x81, 0xfc}, peaks, "peak tile by mask"},
        {kAtlasCall, Rel32(0xE8, base + kAtlasCall, base + kCoastClassFn), Rel32(0xE8, base + kAtlasCall, atlasThunk),
         "coast atlas cell (CALL FUN_004788e0)"},
        {kWaveCall, Rel32(0xE8, base + kWaveCall, base + kCoastClassFn), Rel32(0xE8, base + kWaveCall, waveThunk),
         "coast wave model (CALL FUN_004788e0)"},
    };
}
}  // namespace TileSets
