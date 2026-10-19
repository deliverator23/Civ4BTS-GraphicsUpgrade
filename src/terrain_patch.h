// Experimental, developer only ([debug] TerrainVertsPerPlot): more height samples per plot than BtS's 17.
//
// CIV4TerrainSettings.xml's iGameCellSizeX/Y sets the samples per plot, and the heightmap tiles
// (art/terrain/heightmap/*.tga) must then be that minus 1 pixels square. But two places in Civ4BeyondSword.exe
// hard-code 17 (found in a memory dump of the Steam build, 3.1.9.0; the file itself is encrypted):
//   - the terrain chunk's constructor sets its samples per plot to 17 and its plots to (samples - 1) >> 4, so at 33 a
//     chunk reads layer records past its own plots, gets an empty one and crashes (+0xbb54a);
//   - the river canyon mask is read as a 16x16 window (offset << 4, width and height 16), so river beds need a
//     canyon mask atlas scaled to match (Art/Terrain/Routes/Rivers/Canyons.tga).
// The patch rewrites those immediates, and only if every site still holds BtS's own bytes. Pure logic here (no
// Windows calls), so the test harness can run it on a buffer; terrain_patch.cpp applies it to the running exe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace TerrainPatch
{
struct Site
{
    uint32_t rva;  // from the exe's image base
    std::vector<uint8_t> original, patched;
    const char* what;
};

// Samples per plot BtS itself uses.
constexpr int kStockVerts = 17;

// The sites for this many samples per plot: 17, 33 or 65 (a power of two plus 1). Empty for anything else.
inline std::vector<Site> Sites(int verts)
{
    int shift = 0;
    while ((1 << shift) < verts - 1)
        ++shift;
    if (verts < kStockVerts || verts > 65 || (1 << shift) != verts - 1)
        return {};
    const uint8_t v = static_cast<uint8_t>(verts), cells = static_cast<uint8_t>(verts - 1),
                  s = static_cast<uint8_t>(shift);
    return {
        {0xba862, {0xba, 0x11, 0, 0, 0}, {0xba, v, 0, 0, 0}, "terrain chunk: samples per plot (MOV EDX,17)"},
        {0xba884, {0xc1, 0xe9, 0x04}, {0xc1, 0xe9, s}, "terrain chunk: plots across (SHR ECX,4)"},
        {0xba887, {0xc1, 0xe8, 0x04}, {0xc1, 0xe8, s}, "terrain chunk: plots down (SHR EAX,4)"},
        {0x78db3, {0xc1, 0xe0, 0x04}, {0xc1, 0xe0, s}, "river canyon 1: window offset (SHL EAX,4)"},
        {0x78dc4, {0xb8, 0x10, 0, 0, 0}, {0xb8, cells, 0, 0, 0}, "river canyon 1: window size (MOV EAX,16)"},
        {0x78ef3, {0xc1, 0xe0, 0x04}, {0xc1, 0xe0, s}, "river canyon 2: window offset (SHL EAX,4)"},
        {0x78f04, {0xb8, 0x10, 0, 0, 0}, {0xb8, cells, 0, 0, 0}, "river canyon 2: window size (MOV EAX,16)"},
    };
}

// Checks every site of image (size bytes from the image base) and, only if each holds its original bytes, calls
// write(rva, bytes, count) for each. Sites already patched count as done. Returns "" when the image ends up patched,
// else why nothing was written.
template <class Write>
std::string ApplySites(const uint8_t* image, size_t size, const std::vector<Site>& sites, Write write)
{
    size_t todo = 0;
    for (const Site& site : sites)
    {
        if (site.rva + site.original.size() > size)
            return std::string("the exe is too small for ") + site.what + " (not this BtS build)";
        const uint8_t* at = image + site.rva;
        if (memcmp(at, site.patched.data(), site.patched.size()) == 0)
            continue;
        if (memcmp(at, site.original.data(), site.original.size()) != 0)
        {
            char where[16];
            snprintf(where, sizeof(where), "+0x%x", static_cast<unsigned>(site.rva));
            return std::string("unexpected bytes at ") + where + " (" + site.what + "): not this BtS build";
        }
        ++todo;
    }
    for (const Site& site : sites)
        if (todo && memcmp(image + site.rva, site.patched.data(), site.patched.size()) != 0)
            write(site.rva, site.patched.data(), site.patched.size());
    return "";
}

// The landscape's cell values the exe gets from the DLL (CvLandscapeInfo, from CIV4TerrainSettings.xml), as the patch
// gives them instead, so the XML needn't change: samples per plot = verts; plots per cell = the XML's, but at most so
// many that a cell stays within 129 x 129 samples (4 at 33, 2 at 65); and the samples per cell from those, as the DLL
// works them out (plots x samples - (plots - 1)).
struct CellValues
{
    int samples, plots, vertCnt;
};
inline CellValues Cells(int verts, int xmlPlots)
{
    const int most = (std::max)(1, 128 / (verts - 1));
    const int plots = (std::max)(1, (std::min)(xmlPlots, most));
    return {verts, plots, plots * verts - (plots - 1)};
}
// The delay-load import slots (VAs) the exe reads them through, and the delay-load thunks they hold until resolved.
struct ImportSlot
{
    uint32_t slot, thunk;
    const char* name;  // the DLL export
};
inline std::vector<ImportSlot> CellImports()
{
    return {
        {0xb7845c, 0x8ebfa4, "?getHorizontalVertCnt@CvLandscapeInfo@@QBEHXZ"},
        {0xb78460, 0x8ebfae, "?getVerticalVertCnt@CvLandscapeInfo@@QBEHXZ"},
        {0xb78464, 0x8ebfb8, "?getPlotsPerCellX@CvLandscapeInfo@@QBEHXZ"},
        {0xb78468, 0x8ebfc2, "?getPlotsPerCellY@CvLandscapeInfo@@QBEHXZ"},
        {0xb7846c, 0x8ebfcc, "?getHorizontalGameCell@CvLandscapeInfo@@QBEHXZ"},
        {0xb78470, 0x8ebfd6, "?getVerticalGameCell@CvLandscapeInfo@@QBEHXZ"},
    };
}

// The terrain patch for this many samples per plot (ApplySites); "" when patched (or verts is 17: nothing to do).
template <class Write>
std::string Apply(const uint8_t* image, size_t size, int verts, Write write)
{
    if (verts == kStockVerts)
        return "";
    std::vector<Site> sites = Sites(verts);
    if (sites.empty())
        return "TerrainVertsPerPlot " + std::to_string(verts) + " isn't 17, 33 or 65";
    return ApplySites(image, size, sites, write);
}
}  // namespace TerrainPatch
