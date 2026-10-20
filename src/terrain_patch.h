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
//
// Hills and peaks are also stamped through a coast scale mask (coastblendmasks/coastscalemask00..15.tga, picked by the
// square's water corners: FUN_00478f50, called only from the hill and peak stamps), which fades them towards the sea.
// The engine reads it at the tile's own pixel coordinates with the mask's own row length, so a mask smaller than the
// tiles (a mod with bigger tiles but BtS's 16x16 masks) is read past its end: only the first rows of each hill or peak
// are scaled by the mask, the rest by whatever memory follows it. So the patch also calls the picker through a thunk
// that hands back a copy of the mask resampled to the tile size (MaskSites, ResampleMask).
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

// The coast scale mask picker (FUN_00478f50: EAX = the square's water corners, EDI = where it writes the mask: width,
// height, row bytes, pixels; x and y on the stack, popped by RET 8) and its two calls, from the hill and peak stamps.
constexpr uint32_t kMaskPicker = 0x78f50;
constexpr uint32_t kMaskCalls[2] = {0x78d24, 0x78e39};

inline std::vector<uint8_t> CallTo(uint32_t from, uint32_t to)
{
    const uint32_t rel = to - (from + 5);
    return {0xe8, static_cast<uint8_t>(rel), static_cast<uint8_t>(rel >> 8), static_cast<uint8_t>(rel >> 16),
            static_cast<uint8_t>(rel >> 24)};
}

// The two calls to the picker, sent to thunk instead (base: where the exe is loaded).
inline std::vector<Site> MaskSites(uint32_t base, uint32_t thunk)
{
    std::vector<Site> sites;
    for (uint32_t rva : kMaskCalls)
        sites.push_back({rva, CallTo(base + rva, base + kMaskPicker), CallTo(base + rva, thunk),
                         rva == kMaskCalls[0] ? "hill stamp: coast scale mask (CALL FUN_00478f50)"
                                              : "peak stamp: coast scale mask (CALL FUN_00478f50)"});
    return sites;
}

// An 8-bit mask (w x h, stride bytes a row) resampled to n x n, as the tiles were upscaled: output pixel i reads the
// input at (i + 1/2) (w - 1) / n, linear between the two pixels either side, the end slopes carried on past the ends,
// clamped to 0..255. So the samples are evenly spaced, the same either way round (rotated or mirrored masks line up).
inline void ResampleMask(const uint8_t* src, int w, int h, int stride, int n, uint8_t* out)
{
    auto axis = [n](int i, int len, int& i0, float& t) {
        const float s = len > 1 ? (i + 0.5f) * (len - 1) / n : 0.0f;
        i0 = (std::min)((std::max)(static_cast<int>(s), 0), (std::max)(len - 2, 0));
        t = len > 1 ? s - i0 : 0.0f;
    };
    for (int y = 0; y < n; ++y)
    {
        int y0;
        float ty;
        axis(y, h, y0, ty);
        const int y1 = (std::min)(y0 + 1, h - 1);
        for (int x = 0; x < n; ++x)
        {
            int x0;
            float tx;
            axis(x, w, x0, tx);
            const int x1 = (std::min)(x0 + 1, w - 1);
            const float top = src[y0 * stride + x0] + (src[y0 * stride + x1] - src[y0 * stride + x0]) * tx;
            const float bottom = src[y1 * stride + x0] + (src[y1 * stride + x1] - src[y1 * stride + x0]) * tx;
            const float v = top + (bottom - top) * ty;
            out[y * n + x] = static_cast<uint8_t>((std::min)(255.0f, (std::max)(0.0f, v + 0.5f)));
        }
    }
}

// The heightmap tiles' folder. The tile loader (FUN_00477fa0) hands each list's path prefix to the list reader
// (FUN_00477dc0: "<prefix><n>.tga" for n = 1, 2... until one is missing) with PUSH <string>, and builds the coast scale
// masks' names with sprintf("%s%02d.tga", "...CoastScaleMask", n). With more samples per plot the patch points those
// PUSHes at its own strings in art/Terrain/heightmap<cells>/ (heightmap64 at 65, heightmap32 at 33), for each list
// whose first file is there; the others keep art/Terrain/heightmap/. So a mod can ship its 16-px tiles where they've
// always been, for players without the patch, and its bigger ones beside them.
struct TilePrefix
{
    uint32_t pushRva;     // PUSH imm32 (68 xx xx xx xx)
    uint32_t stringVa;    // the string it pushes
    const char* prefix;   // that string (the scale masks: the prefix their format is given)
    const char* first;    // the list's first file after the prefix
    const char* what;
};
inline std::vector<TilePrefix> TilePrefixes()
{
    return {
        {0x77faa, 0xc3cad0, "art/Terrain/heightmap/Ocean1_", "1.tga", "ocean"},
        {0x77fb9, 0xc3caac, "art/Terrain/heightmap/flats/flat1_", "1.tga", "flats"},
        {0x77fc8, 0xc3ca88, "art/Terrain/heightmap/hills/hill1_", "1.tga", "hills"},
        {0x77fd7, 0xc3ca64, "art/Terrain/heightmap/peaks/Peak_", "1.tga", "peaks"},
        {0x77fe6, 0xc3ca38, "art/Terrain/heightmap/Coasts/CoastTile1_0", "1.tga", "coasts 1"},
        {0x77ff5, 0xc3ca0c, "art/Terrain/heightmap/Coasts/CoastTile3_0", "1.tga", "coasts 3"},
        {0x78004, 0xc3c9e0, "art/Terrain/heightmap/Coasts/CoastTile7_0", "1.tga", "coasts 7"},
        {0x78013, 0xc3c9b4, "art/Terrain/heightmap/Coasts/CoastTile5_0", "1.tga", "coasts 5"},
        // the scale masks' sprintf format ("%s%02d.tga"): ours prints the new prefix and skips the old one's argument
        {0x7804a, 0xc3c970, "art/Terrain/heightmap/CoastBlendMasks/CoastScaleMask", "00.tga", "coast scale masks"},
    };
}
// A path in the folder for this many samples per plot: "heightmap/" -> "heightmap<verts - 1>/".
inline std::string InTileFolder(const std::string& path, int verts)
{
    const std::string from = "heightmap/";
    const size_t at = path.find(from);
    if (at == std::string::npos)
        return path;
    return path.substr(0, at) + "heightmap" + std::to_string(verts - 1) + "/" + path.substr(at + from.size());
}
// What the patched PUSH points at: the list's prefix in the new folder, or for the scale masks a format with it.
inline std::string TileString(const TilePrefix& p, int verts)
{
    const std::string moved = InTileFolder(p.prefix, verts);
    return p.stringVa == 0xc3c970 ? "%.0s" + moved + "%02d.tga" : moved;
}
inline std::vector<Site> TileFolderSites(uint32_t base, const std::vector<TilePrefix>& prefixes,
                                         const std::vector<uint32_t>& strings)
{
    std::vector<Site> sites;
    for (size_t k = 0; k < prefixes.size(); ++k)
    {
        const uint32_t from = prefixes[k].stringVa - 0x400000 + base, to = strings[k];
        sites.push_back({prefixes[k].pushRva,
                         {0x68, uint8_t(from), uint8_t(from >> 8), uint8_t(from >> 16), uint8_t(from >> 24)},
                         {0x68, uint8_t(to), uint8_t(to >> 8), uint8_t(to >> 16), uint8_t(to >> 24)},
                         prefixes[k].what});
    }
    return sites;
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
