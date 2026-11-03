// Where the terrain patches' code and data are in each known build of Civ4BeyondSword.exe (3.1.9.0, image base
// 0x400000). Steam's and GOG's exes are the same code laid out differently: Steam's (linked 2014, encrypted on disk by
// SteamStub, so its addresses came from a memory dump) and GOG's (linked 2009, plain on disk). The running build is
// recognised by three calls into the tile variant picker, which no patch writes over; every patch still checks BtS's
// own bytes at each of its sites before writing, so a build that isn't listed here is left alone. Pure data and logic
// (no Windows calls), so the test harness can run it on a buffer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Exe
{
struct Layout
{
    const char* name;
    // TerrainVertsPerPlot (terrain_patch.h). RVAs unless marked VA.
    uint32_t chunkVerts, chunkPlotsX, chunkPlotsY;  // the terrain chunk: MOV EDX,17; SHR ECX,4; SHR EAX,4
    uint32_t canyonShift[2], canyonSize[2];         // the river canyon reads: SHL EAX,4; MOV EAX,16
    uint32_t maskPicker, maskPickerEnd;             // FUN_00478f50 (Steam's numbering), the coast scale mask picker
    uint32_t maskCalls[2];                          // its calls from the hill and peak stamps
    uint32_t tilePushes[9];   // the tile loader's PUSH <prefix>, in TerrainPatch::TilePrefixes' order
    uint32_t tileStrings[9];  // VAs: the strings they push
    uint32_t cellSlots[6];    // VAs: the six CvLandscapeInfo getters' delay-load import slots (TerrainPatch::CellImports)
    uint32_t cellThunks[6];   // VAs: the delay-load thunks those slots hold until resolved
    // Terrain tile groups (terrain_sets.h), land blends and jigsaw pieces.
    uint32_t picker;          // FUN_00477ef0, the variant picker
    uint32_t listCaller;      // return address of its call from the list reader (FUN_00478180; the list in ESI)
    uint32_t coast1Caller;    // return address of its call for the one-land-corner coasts (in FUN_00478a90)
    uint32_t blendCaller;     // return address of its call choosing terrain blend textures
    uint32_t hills, peaks;    // the hill and peak tile lookups by corner mask
    uint32_t lists[8];        // VAs: the tile lists (std::vector), in the loader's order: ocean, flats, hills, peaks,
                              // coasts 1, 3, 7, 5
    uint32_t srand, rand;     // VAs: the CRT's srand and rand import slots
    uint32_t coastClassFn, coastClassFnEnd;  // FUN_004788e0, a coast square's variant index
    uint32_t atlasCall, waveCall;            // its calls for the coast atlas cell and the wave model
    uint32_t blendPick;                      // FUN_00479920, a terrain layer's blend pick
    uint32_t blendPickCalls[3];
    uint32_t baseStamp;                      // FUN_00478a90, the base tile stamp
};

// Steam's build (timestamp 0x53869b61).
const Layout kSteam = {
    "Steam",
    0xba862, 0xba884, 0xba887,
    {0x78db3, 0x78ef3}, {0x78dc4, 0x78f04},
    0x78f50, 0x78fd0,
    {0x78d24, 0x78e39},
    {0x77faa, 0x77fb9, 0x77fc8, 0x77fd7, 0x77fe6, 0x77ff5, 0x78004, 0x78013, 0x7804a},
    {0xc3cad0, 0xc3caac, 0xc3ca88, 0xc3ca64, 0xc3ca38, 0xc3ca0c, 0xc3c9e0, 0xc3c9b4, 0xc3c970},
    {0xb7845c, 0xb78460, 0xb78464, 0xb78468, 0xb7846c, 0xb78470},
    {0x8ebfa4, 0x8ebfae, 0x8ebfb8, 0x8ebfc2, 0x8ebfcc, 0x8ebfd6},
    0x77ef0, 0x781a1, 0x78c40, 0x79979,
    0x78ce0, 0x78e20,
    {0xb85340, 0xb85350, 0xb85360, 0xb85370, 0xb85380, 0xb85390, 0xb853a0, 0xb853b0},
    0xa5a4e0, 0xa5a4dc,
    0x788e0, 0x789c0,
    0xcdec2, 0x211a18,
    0x79920,
    {0x796b8, 0x79787, 0x79845},
    0x78a90,
};

// GOG's build (timestamp 0x4a0c27e6).
const Layout kGog = {
    "GOG",
    0xba622, 0xba644, 0xba647,
    {0x78a13, 0x78b53}, {0x78a24, 0x78b64},
    0x78bb0, 0x78c30,
    {0x78984, 0x78a99},
    {0x77c0a, 0x77c19, 0x77c28, 0x77c37, 0x77c46, 0x77c55, 0x77c64, 0x77c73, 0x77caa},
    {0xc99aa8, 0xc99a84, 0xc99a60, 0xc99a3c, 0xc99a10, 0xc999e4, 0xc999b8, 0xc9998c, 0xc99948},
    {0xbc1fac, 0xbc1fb0, 0xbc1fb4, 0xbc1fb8, 0xbc1fbc, 0xbc1fc0},
    {0x8ee673, 0x8ee67d, 0x8ee687, 0x8ee691, 0x8ee69b, 0x8ee6a5},
    0x77b50, 0x77e01, 0x788a0, 0x795d9,
    0x78940, 0x78a80,
    {0xbe2b38, 0xbe2b48, 0xbe2b58, 0xbe2b68, 0xbe2b78, 0xbe2b88, 0xbe2b98, 0xbe2ba8},
    0xa9c474, 0xa9c470,
    0x78540, 0x78620,
    0xcdac2, 0x211c38,
    0x79580,
    {0x79318, 0x793e7, 0x794a5},
    0x786f0,
};

const Layout* const kLayouts[] = {&kSteam, &kGog};

// Whether the call returning to ret (an RVA) is CALL target.
inline bool CallsTo(const uint8_t* image, size_t size, uint32_t ret, uint32_t target)
{
    if (ret < 5 || ret > size || image[ret - 5] != 0xe8)
        return false;
    int32_t rel;
    memcpy(&rel, image + ret - 4, 4);
    return ret + static_cast<uint32_t>(rel) == target;
}

// The layout of the exe image (size bytes from its base, laid out as loaded), or nullptr for a build not listed.
inline const Layout* Identify(const uint8_t* image, size_t size)
{
    for (const Layout* l : kLayouts)
        if (CallsTo(image, size, l->listCaller, l->picker) && CallsTo(image, size, l->coast1Caller, l->picker) &&
            CallsTo(image, size, l->blendCaller, l->picker))
            return l;
    return nullptr;
}
}  // namespace Exe
