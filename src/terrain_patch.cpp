// [debug] TerrainVertsPerPlot: patches the running Civ4BeyondSword.exe (see terrain_patch.h). Runs from Init, on the
// game's first Direct3DCreate9: the game loads d3d9.dll itself, after SteamStub has decrypted its code, and before it
// builds any terrain.
#include "common.h"
#include "terrain_patch.h"

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
    Log::Write("terrain patch: %d samples per plot; %d of %u code sites written (exe timestamp 0x%08lx); %s. "
               "Needs %dx%d heightmap tiles",
               verts, written, static_cast<unsigned>(TerrainPatch::Sites(verts).size()),
               static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
               imports.empty() ? "the landscape's cell sizes come from the patch, not CIV4TerrainSettings.xml"
                               : ("CIV4TerrainSettings.xml's cell sizes used as they are: " + imports).c_str(),
               verts - 1, verts - 1);
}
