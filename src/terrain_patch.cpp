// [debug] TerrainVertsPerPlot: patches the running Civ4BeyondSword.exe (see terrain_patch.h). Runs from Init, on the
// game's first Direct3DCreate9: the game loads d3d9.dll itself, after SteamStub has decrypted its code, and before it
// builds any terrain.
#include "assets.h"
#include "common.h"
#include "terrain_patch.h"

#include <memory>

namespace
{
// The landscape's cell values, as the patch gives them (TerrainPatch::Cells): the exe's import slots for the six
// CvLandscapeInfo getters point here, so CIV4TerrainSettings.xml needn't change. The DLL's own plots-per-cell getters
// are found by name: calling the slots' delay-load thunks would write the real functions back over these.
using Getter = int(__fastcall*)(void* self, void* unused);  // thiscall, no arguments
Getter g_realPlots[2] = {}, g_realCell = nullptr;
bool g_resolved = false, g_loggedCells = false;

TerrainPatch::CellValues CellsFor(void* landscape, int axis)
{
    if (!g_resolved)
    {
        g_resolved = true;
        if (HMODULE dll = GetModuleHandleW(L"CvGameCoreDLL.dll"))
        {
            g_realPlots[0] = reinterpret_cast<Getter>(GetProcAddress(dll, "?getPlotsPerCellX@CvLandscapeInfo@@QBEHXZ"));
            g_realPlots[1] = reinterpret_cast<Getter>(GetProcAddress(dll, "?getPlotsPerCellY@CvLandscapeInfo@@QBEHXZ"));
            g_realCell = reinterpret_cast<Getter>(GetProcAddress(dll, "?getHorizontalGameCell@CvLandscapeInfo@@QBEHXZ"));
        }
    }
    const int xmlPlots = g_realPlots[axis] ? g_realPlots[axis](landscape, nullptr) : 4;
    const TerrainPatch::CellValues c = TerrainPatch::Cells(g_config.terrainVertsPerPlot, xmlPlots);
    if (!g_loggedCells)
    {
        g_loggedCells = true;
        Log::Write("terrain patch: the landscape read as %d samples per plot and %d plots per cell (%d samples a cell); "
                   "CIV4TerrainSettings.xml says %d plots per cell, iGameCellSize %d (not used)",
                   c.samples, c.plots, c.vertCnt, xmlPlots, g_realCell ? g_realCell(landscape, nullptr) : -1);
    }
    return c;
}
int __fastcall HorizontalVertCnt(void* self, void*) { return CellsFor(self, 0).vertCnt; }
int __fastcall VerticalVertCnt(void* self, void*) { return CellsFor(self, 1).vertCnt; }
int __fastcall PlotsPerCellX(void* self, void*) { return CellsFor(self, 0).plots; }
int __fastcall PlotsPerCellY(void* self, void*) { return CellsFor(self, 1).plots; }
int __fastcall GameCellX(void* self, void*) { return CellsFor(self, 0).samples; }
int __fastcall GameCellY(void* self, void*) { return CellsFor(self, 1).samples; }

// Points the six import slots at the functions above; only if each still holds its delay-load thunk or the DLL's own
// function. Returns "" when done.
std::string HookCellImports()
{
    const Getter ours[6] = {HorizontalVertCnt, VerticalVertCnt, PlotsPerCellX, PlotsPerCellY, GameCellX, GameCellY};
    const auto slots = TerrainPatch::CellImports();
    HMODULE dll = GetModuleHandleW(L"CvGameCoreDLL.dll");
    for (const auto& s : slots)
    {
        const uint32_t now = *reinterpret_cast<const uint32_t*>(s.slot);
        const uint32_t real = dll ? reinterpret_cast<uint32_t>(GetProcAddress(dll, s.name)) : 0;
        if (now != s.thunk && (real == 0 || now != real))
        {
            char why[160];
            snprintf(why, sizeof(why), "import slot 0x%08x holds 0x%08x, not its delay-load thunk 0x%08x", s.slot, now,
                     s.thunk);
            return why;
        }
    }
    for (size_t k = 0; k < slots.size(); ++k)
    {
        void* at = reinterpret_cast<void*>(slots[k].slot);
        DWORD old = 0;
        if (!VirtualProtect(at, 4, PAGE_READWRITE, &old))
            return "import slots can't be written";
        *reinterpret_cast<uint32_t*>(at) = reinterpret_cast<uint32_t>(ours[k]);
        VirtualProtect(at, 4, old, &old);
    }
    return "";
}
}  // namespace

// Hills' and peaks' coast scale masks (see terrain_patch.h): the engine's pick, then, when it isn't the tiles' size, a
// copy resampled to it (made once per mask and kept: there are 16).
extern "C" uint32_t g_terrainMaskPicker = 0;  // FUN_00478f50, called by the thunk below
namespace
{
struct ResampledMask
{
    const uint8_t* from;
    int w, h;
    std::vector<uint8_t> pixels;
};
std::vector<std::unique_ptr<ResampledMask>> g_masks;
bool g_loggedMaskFormat = false;
}  // namespace

extern "C" void __stdcall TerrainMaskFix(int32_t* mask)
{
    const int n = g_config.terrainVertsPerPlot - 1;
    const int w = mask[0], h = mask[1], stride = mask[2];
    const uint8_t* from = reinterpret_cast<const uint8_t*>(mask[3]);
    if (!from || w <= 0 || h <= 0 || (w == n && h == n))
        return;
    if (stride != w)
    {
        if (!g_loggedMaskFormat)
            Log::Write("terrain patch: a coast scale mask is %dx%d with %d bytes a row, not 8-bit: hills and peaks read "
                       "it as it is (it should be %dx%d)", w, h, stride, n, n);
        g_loggedMaskFormat = true;
        return;
    }
    ResampledMask* found = nullptr;
    for (const auto& m : g_masks)
        if (m->from == from && m->w == w && m->h == h)
            found = m.get();
    if (!found)
    {
        g_masks.push_back(std::make_unique<ResampledMask>(ResampledMask{from, w, h, std::vector<uint8_t>(n * n)}));
        found = g_masks.back().get();
        TerrainPatch::ResampleMask(from, w, h, stride, n, found->pixels.data());
        Log::Write("terrain patch: coast scale mask %u is %dx%d, the hill and peak tiles %dx%d: resampled to match "
                   "(Art/Terrain/heightmap/coastblendmasks/coastscalemask*.tga %dx%d would be used as they are)",
                   static_cast<unsigned>(g_masks.size()), w, h, n, n, n, n);
    }
    mask[0] = mask[1] = mask[2] = n;
    mask[3] = reinterpret_cast<int32_t>(found->pixels.data());
}

extern "C" __declspec(naked) void TerrainMaskThunk()
{
    __asm {
        push dword ptr [esp + 8]  // y
        push dword ptr [esp + 8]  // x
        call dword ptr [g_terrainMaskPicker]  // EAX (water corners) and EDI (the mask) as the caller set them; pops x, y
        push edi
        call TerrainMaskFix
        ret 8
    }
}

void ApplyTerrainPatch()
{
    const int verts = g_config.terrainVertsPerPlot;
    if (verts == TerrainPatch::kStockVerts)
        return;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = exe;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    for (auto& c : name)
        c = static_cast<wchar_t>(towlower(c));
    if (name != L"civ4beyondsword.exe")
    {
        Log::Write("terrain patch: TerrainVertsPerPlot %d not applied: %ls isn't Civ4BeyondSword.exe", verts, name.c_str());
        return;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    int written = 0;
    std::string why = TerrainPatch::Apply(base, size, verts, [&](uint32_t rva, const uint8_t* bytes, size_t count) {
        void* at = const_cast<uint8_t*>(base) + rva;
        DWORD old = 0;
        if (VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(at, bytes, count);
            VirtualProtect(at, count, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, count);
            ++written;
        }
    });
    if (!why.empty())
    {
        Log::Write("terrain patch: TerrainVertsPerPlot %d not applied: %s (exe timestamp 0x%08lx)", verts, why.c_str(),
                   static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return;
    }
    const std::string imports = HookCellImports();
    // Hills' and peaks' coast scale masks: resampled to the tile size when they aren't (the exe at its usual base).
    std::string masks = "not at the usual base";
    if (reinterpret_cast<uint32_t>(base) == 0x400000)
    {
        g_terrainMaskPicker = reinterpret_cast<uint32_t>(base) + TerrainPatch::kMaskPicker;
        int maskWrites = 0;
        masks = TerrainPatch::ApplySites(base, size, TerrainPatch::MaskSites(reinterpret_cast<uint32_t>(base),
                                                                              reinterpret_cast<uint32_t>(&TerrainMaskThunk)),
                                         [&](uint32_t rva, const uint8_t* bytes, size_t count) {
            void* at = const_cast<uint8_t*>(base) + rva;
            DWORD old = 0;
            if (VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(at, bytes, count);
                VirtualProtect(at, count, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, count);
                ++maskWrites;
            }
        });
        if (masks.empty())
            masks = std::to_string(maskWrites) + " of 2 written";
    }
    Log::Write("terrain patch: hills' and peaks' coast scale masks fitted to the tiles: %s", masks.c_str());

    // The heightmap tiles from art/Terrain/heightmap<cells>/, for each list whose first file is there (loose or packed,
    // the mod's first); the others where they always were. Counted for the log as the engine reads them (1, 2...).
    if (reinterpret_cast<uint32_t>(base) == 0x400000)
    {
        static std::vector<std::string> strings;  // pushed by the patched code: kept for the game's lifetime
        const auto prefixes = TerrainPatch::TilePrefixes();
        std::vector<TerrainPatch::TilePrefix> moved;
        std::vector<uint32_t> movedStrings;
        std::string fromNew, fromOld;
        std::vector<uint8_t> bytes;
        std::wstring where;
        strings.reserve(prefixes.size());
        for (const auto& p : prefixes)
        {
            const std::string prefix = TerrainPatch::InTileFolder(p.prefix, verts);
            const bool scale = p.stringVa == 0xc3c970;
            auto name = [&](int n) {
                char num[16];
                snprintf(num, sizeof(num), scale ? "%02d.tga" : "%d.tga", n);
                return prefix + num;
            };
            const bool there = ReadAsset(Widen(name(scale ? 0 : 1)), bytes, where);
            if (!there)
            {
                fromOld += (fromOld.empty() ? "" : ", ") + std::string(p.what);
                continue;
            }
            int count = 1;  // the first is there
            while (count < 256 && ReadAsset(Widen(name(scale ? count : count + 1)), bytes, where))
                ++count;
            strings.push_back(TerrainPatch::TileString(p, verts));
            moved.push_back(p);
            movedStrings.push_back(reinterpret_cast<uint32_t>(strings.back().c_str()));
            fromNew += (fromNew.empty() ? "" : ", ") + std::string(p.what) + " " + std::to_string(count);
        }
        std::string why = moved.empty() ? "" : TerrainPatch::ApplySites(base, size, TerrainPatch::TileFolderSites(
            reinterpret_cast<uint32_t>(base), moved, movedStrings), [&](uint32_t rva, const uint8_t* b, size_t count) {
            void* at = const_cast<uint8_t*>(base) + rva;
            DWORD old = 0;
            if (VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(at, b, count);
                VirtualProtect(at, count, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, count);
            }
        });
        const std::string folder = "art/Terrain/heightmap" + std::to_string(verts - 1) + "/";
        if (!why.empty())
            Log::Write("terrain patch: heightmap tiles from %s not used: %s", folder.c_str(), why.c_str());
        else
            Log::Write("terrain patch: heightmap tiles from %s: %s; from art/Terrain/heightmap/: %s", folder.c_str(),
                       fromNew.empty() ? "none" : fromNew.c_str(), fromOld.empty() ? "none" : fromOld.c_str());
    }
    Log::Write("terrain patch: %d samples per plot; %d of %u code sites written (exe timestamp 0x%08lx); %s. "
               "Needs %dx%d heightmap tiles",
               verts, written, static_cast<unsigned>(TerrainPatch::Sites(verts).size()),
               static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
               imports.empty() ? "the landscape's cell sizes come from the patch, not CIV4TerrainSettings.xml"
                               : ("CIV4TerrainSettings.xml's cell sizes used as they are: " + imports).c_str(),
               verts - 1, verts - 1);
}
