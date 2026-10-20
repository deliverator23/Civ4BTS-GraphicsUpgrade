// [terrain tiles] Patch, Sets, Set1..Set8: heightmap tile sets by terrain, and hill/peak variants (terrain_sets.h). The
// INI and the log number sets from 1 (Set1 = set 0 here); everything inside is 0-based.
//
// Three hooks in the running Civ4BeyondSword.exe, written only if every site holds BtS's own bytes:
//   - the variant picker jumps to PickerThunk (ECX = x, EAX = y, [ESP+4] = lo, [ESP+8] = hi, caller cleans);
//   - the hill and peak lookups call HillThunk / PeakThunk (EAX = corner mask, EBP = x, EBX = y), which return the
//     tile in EDX / ESI.
// Map, plot and terrain come from the game DLL's exports (thiscall, called as fastcall with an unused EDX).
//
// [terrain tiles] HillField (experimental, hill_field.h): the hill lookup hands back a tile cut from one map-wide
// field of hill stamps instead of hill1_<mask>.
#include "common.h"
#include "hill_field.h"
#include "terrain_sets.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
// The game DLL's functions (resolved on first use: the DLL is loaded by the time a map is built).
struct GameDll
{
    using GetInstance = void*(__cdecl*)();
    using Member = void*(__fastcall*)(void* self, void* unused);
    using MemberInt = int(__fastcall*)(void* self, void* unused);
    using MemberBool = bool(__fastcall*)(void* self, void* unused);
    using MemberArg = void*(__fastcall*)(void* self, void* unused, int a);
    using MemberArgs = void*(__fastcall*)(void* self, void* unused, int a, int b);
    using MemberStr = const char*(__fastcall*)(void* self, void* unused);
    GetInstance getInstance = nullptr;
    Member getMap = nullptr;
    MemberInt width = nullptr, height = nullptr, numTerrains = nullptr, terrainType = nullptr;
    MemberBool wrapX = nullptr, wrapY = nullptr, isWater = nullptr, isHills = nullptr;
    MemberArgs plot = nullptr;
    MemberArg terrainInfo = nullptr;
    MemberStr type = nullptr;
    bool tried = false, ok = false;

    bool Resolve()
    {
        if (tried)
            return ok;
        tried = true;
        HMODULE dll = GetModuleHandleW(L"CvGameCoreDLL.dll");
        if (!dll)
            return false;
        auto get = [&](const char* name) { return GetProcAddress(dll, name); };
        getInstance = reinterpret_cast<GetInstance>(get("?getInstance@CvGlobals@@SAAAV1@XZ"));
        getMap = reinterpret_cast<Member>(get("?getMap@CvGlobals@@QAEAAVCvMap@@XZ"));
        width = reinterpret_cast<MemberInt>(get("?getGridWidth@CvMap@@QBEHXZ"));
        height = reinterpret_cast<MemberInt>(get("?getGridHeight@CvMap@@QBEHXZ"));
        wrapX = reinterpret_cast<MemberBool>(get("?isWrapX@CvMap@@QAE_NXZ"));
        wrapY = reinterpret_cast<MemberBool>(get("?isWrapY@CvMap@@QAE_NXZ"));
        plot = reinterpret_cast<MemberArgs>(get("?plot@CvMap@@QBEPAVCvPlot@@HH@Z"));
        terrainType = reinterpret_cast<MemberInt>(get("?getTerrainType@CvPlot@@QBE?AW4TerrainTypes@@XZ"));
        isWater = reinterpret_cast<MemberBool>(get("?isWater@CvPlot@@QBE_NXZ"));  // optional
        isHills = reinterpret_cast<MemberBool>(get("?isHills@CvPlot@@QBE_NXZ"));  // optional: the hill field
        numTerrains = reinterpret_cast<MemberInt>(get("?getNumTerrainInfos@CvGlobals@@QAEHXZ"));
        terrainInfo = reinterpret_cast<MemberArg>(get("?getTerrainInfo@CvGlobals@@QAEAAVCvTerrainInfo@@W4TerrainTypes@@@Z"));
        type = reinterpret_cast<MemberStr>(get("?getType@CvInfoBase@@QBEPBDXZ"));
        ok = getInstance && getMap && width && height && wrapX && wrapY && plot && terrainType && numTerrains &&
             terrainInfo && type;
        if (!ok)
            Log::Write("terrain tile sets: the game DLL lacks the map/plot/terrain exports; every square uses set 0");
        return ok;
    }
    void* Map() { return getMap(getInstance(), nullptr); }
};
GameDll g_dll;

uint32_t g_base = 0;
int g_sets = 1;
const int kUnlisted = -2;
std::vector<int> g_terrainSet;  // by TerrainTypes: the set (0-based; the INI's Set1 is 0) it's listed in, or kUnlisted
bool g_tableBuilt = false;
unsigned g_picks[TileSets::kMaxSets] = {}, g_hillVariants[16] = {}, g_peakVariants[16] = {};
unsigned g_logged = 0;

// Which list a pick was for: FUN_00478180 (return +0x781a1) gets it in ESI; the other callers by where they return.
enum List
{
    kOcean, kFlats, kCoast1, kCoast3, kCoast5, kCoast7, kCoastClass, kScale, kOther, kLists
};
const char* const kListNames[kLists] = {"ocean",       "flats",       "coast 1",        "coast 3",    "coast 5",
                                        "coast 7",     "coast class (waves, atlas)", "scale mask", "other"};
unsigned g_listPicks[kLists][TileSets::kMaxSets] = {};
struct Square
{
    int x0, y0;  // its corner plots: (x0, y0) to (x0 + 1, y0 + 1)
    int set;
};
std::vector<Square> g_setFlats;  // the first few flat squares of a set other than 0, to go and look at
bool g_setFlatsLogged = false;

List ListOf(uint32_t returnAddress, uint32_t esi)
{
    const uint32_t rva = returnAddress - g_base;
    if (rva == 0x781a1)  // FUN_00478180: the list object in ESI
        switch (esi)
        {
        case 0xb85340: return kOcean;
        case 0xb85350: return kFlats;
        case 0xb85380: return kCoast1;
        case 0xb85390: return kCoast3;
        case 0xb853a0: return kCoast7;
        case 0xb853b0: return kCoast5;
        default: return kOther;
        }
    if (rva == 0x78c40)  // FUN_00478a90's one-land-corner coasts
        return kCoast1;
    if (rva >= 0x788e0 && rva < 0x789c0)  // FUN_004788e0: the coast class index the waves and atlas use
        return kCoastClass;
    if (rva >= 0x78f50 && rva < 0x78fd0)  // FUN_00478f50: the coast scale mask
        return kScale;
    return kOther;
}

void BuildTable()
{
    g_tableBuilt = true;
    if (!g_dll.Resolve())
        return;
    void* gc = g_dll.getInstance();
    const int n = g_dll.numTerrains(gc, nullptr);
    g_terrainSet.assign(n > 0 ? n : 0, kUnlisted);
    std::string listed;
    for (int t = 0; t < n; ++t)
    {
        const char* name = g_dll.type(g_dll.terrainInfo(gc, nullptr, t), nullptr);
        for (int s = 0; s < g_sets && name; ++s)
        {
            // Set<s + 1>: terrain types separated by commas, semicolons or spaces, any case
            std::string list;
            for (wchar_t c : g_config.tileSetTerrains[s])
                list += static_cast<char>(towupper(c));
            for (char& c : list)
                if (c == ',' || c == ';' || c == '\t')
                    c = ' ';
            std::string upper(name);
            for (char& c : upper)
                c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (g_terrainSet[t] == kUnlisted && (" " + list + " ").find(" " + upper + " ") != std::string::npos)
            {
                g_terrainSet[t] = s;
                listed += (listed.empty() ? "" : ", ") + upper + " Set" + std::to_string(s + 1);
            }
        }
    }
    Log::Write("terrain tile sets: %d sets; %s; other land Set1, other water no vote", g_sets,
               listed.empty() ? "no terrain listed" : listed.c_str());
}

// The set a square's tiles come from: the majority of its corner plots' terrains.
int SquareSet(int x, int y)
{
    if (g_sets <= 1)
        return 0;
    if (!g_tableBuilt)
        BuildTable();
    if (!g_dll.ok)
        return 0;
    void* map = g_dll.Map();
    int corners[4][2], votes[4];
    TileSets::Corners(x, y, g_dll.height(map, nullptr), corners);
    for (int k = 0; k < 4; ++k)
    {
        void* plot = g_dll.plot(map, nullptr, corners[k][0], corners[k][1]);
        votes[k] = -1;
        if (!plot)
            continue;
        const int t = g_dll.terrainType(plot, nullptr);
        if (t < 0 || t >= static_cast<int>(g_terrainSet.size()))
            continue;
        votes[k] = g_terrainSet[t];
        if (votes[k] == kUnlisted)  // land listed nowhere: Set1; water listed nowhere doesn't vote
            votes[k] = g_dll.isWater && g_dll.isWater(plot, nullptr) ? -1 : 0;
    }
    return TileSets::Majority(votes, g_sets);
}

// The engine's wrap: the square past the last column (row) is the first, on a map that wraps that way.
void Wrap(int& x, int& y)
{
    if (!g_dll.Resolve())
        return;
    void* map = g_dll.Map();
    if (x == g_dll.width(map, nullptr) && g_dll.wrapX(map, nullptr))
        x = 0;
    if (y == g_dll.height(map, nullptr) && g_dll.wrapY(map, nullptr))
        y = 0;
}

// MSVC's rand() after srand(seed), without touching the CRT's own state (for the hill and peak variants, which the
// engine never picked at random).
int RandAfter(unsigned seed)
{
    return static_cast<int>(((seed * 214013u + 2531011u) >> 16) & 0x7FFF);
}
}  // namespace

// [terrain tiles] HillField: the map's hills as one field of stamps (hill_field.h). The engine stamps hills square by
// square, row by row (a whole map from GenerateTerrainHeights; the four squares round a changed plot after that), so
// whenever a call comes before the last one, a new pass has begun: the map's hills are read again and the fit brought
// up to date before any of its tiles is cut. The tile is the engine's to read at once, so one serves every call.
HillField::Field g_hillField;
HillField::EngineTile g_hillTile;
bool g_hillStampsSet = false, g_hillLoggedOff = false;
int g_hillLastX = -1, g_hillLastY = -1;
unsigned g_hillCut = 0, g_hillStock = 0, g_hillRefits = 0, g_hillUpdates = 0;

bool HillSync()
{
    void* map = g_dll.Map();
    HillField::Map m;
    m.width = g_dll.width(map, nullptr);
    m.height = g_dll.height(map, nullptr);
    m.wrapX = g_dll.wrapX(map, nullptr);
    m.wrapY = g_dll.wrapY(map, nullptr);
    if (m.width <= 0 || m.height <= 0 || m.width > 1024 || m.height > 1024)
        return false;
    m.hills.assign(static_cast<size_t>(m.width) * m.height, 0);
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            if (void* plot = g_dll.plot(map, nullptr, x, y))
                m.hills[static_cast<size_t>(y) * m.width + x] = g_dll.isHills(plot, nullptr) ? 1 : 0;
    const int how = g_hillField.Update(m);
    if (how == 2)
    {
        ++g_hillRefits;
        int sizes[4] = {}, hills = 0;
        for (const auto& p : g_hillField.Placements())
            ++sizes[(std::min)(3, static_cast<int>(p.frame.size()))];
        for (uint8_t h : m.hills)
            hills += h;
        Log::Write("terrain tiles: hill field fitted to a %dx%d map (%d hill plots): %d three-plot, %d two-plot and %d "
                   "one-plot stamps",
                   m.width, m.height, hills, sizes[3], sizes[2], sizes[1]);
    }
    else if (how == 1)
        ++g_hillUpdates;
    return true;
}

// Square (x, y)'s hill tile from the field, or null for the engine's own (stock: its size and format).
void* HillFieldTile(int mask, int x, int y, void* stock)
{
    if (!g_dll.Resolve() || !g_dll.isHills)
    {
        if (!g_hillLoggedOff)
            Log::Write("terrain tiles: HillField off: the game DLL lacks CvPlot::isHills");
        g_hillLoggedOff = true;
        return nullptr;
    }
    if (!g_hillStampsSet)
    {
        g_hillStampsSet = true;
        g_hillField.SetStamps(HillField::SyntheticStamps());
        Log::Write("terrain tiles: HillField on, with %u made-up stamps (synthetic_*; gain %.2f)",
                   static_cast<unsigned>(g_hillField.Stamps().size()), g_hillField.Gain());
    }
    int w = 0, h = 0, bpp = 0;
    if (!HillField::TileFormat(stock, w, h, bpp) || w != h || bpp != 1 || w < 2 || w > 256)
    {
        ++g_hillStock;
        return nullptr;
    }
    if (!g_hillField.Built() || y < g_hillLastY || (y == g_hillLastY && x <= g_hillLastX))
        HillSync();
    g_hillLastX = x;
    g_hillLastY = y;
    if (g_hillField.Mask(x, y) != mask && (!HillSync() || g_hillField.Mask(x, y) != mask))
    {
        ++g_hillStock;
        return nullptr;
    }
    if (!g_hillField.Tile(x, y, w, g_hillTile.Pixels(w)))
    {
        ++g_hillStock;
        return nullptr;
    }
    ++g_hillCut;
    return g_hillTile.Texture();
}

// The engine's picker, set-aware. Same seed and the exe's own srand/rand, so with one set it picks what BtS picks.
extern "C" int __stdcall TileSetsPicker(int x, int y, int lo, int hi, uint32_t returnAddress, uint32_t esi)
{
    int wx = x, wy = y;
    Wrap(wx, wy);
    using Srand = void(__cdecl*)(unsigned);
    using Rand = int(__cdecl*)();
    (*reinterpret_cast<Srand*>(TileSets::kSrand))(TileSets::Seed(wx, wy));
    const int r = (*reinterpret_cast<Rand*>(TileSets::kRand))();
    const int count = hi - lo;
    if (count <= 0)
        return lo;
    if (g_sets <= 1 || returnAddress == g_base + TileSets::kBlendCaller)
        return lo + r % count;
    const int set = SquareSet(x, y);
    ++g_picks[set];
    const List list = ListOf(returnAddress, esi);
    ++g_listPicks[list][set];
    if (list == kFlats && set > 0 && g_setFlats.size() < 12 && g_dll.ok)
    {
        int corners[4][2];
        TileSets::Corners(x, y, g_dll.height(g_dll.Map(), nullptr), corners);
        const Square sq = {corners[3][0], corners[3][1], set};  // BL: the lowest x and y of the four
        bool seen = false;
        for (const Square& s : g_setFlats)
            seen = seen || (s.x0 == sq.x0 && s.y0 == sq.y0);
        if (!seen)
            g_setFlats.push_back(sq);
    }
    const TileSets::Range range = TileSets::SetRange(count, g_sets, set);
    return lo + range.lo + r % range.size;
}

// A hill (family 0) or peak (1) tile for a corner mask: hill1_<mask + 15 v>, v a variant from the square's set.
extern "C" void* __stdcall TileSetsTile(int family, int mask, int x, int y)
{
    const uint32_t list = family ? TileSets::kPeakList : TileSets::kHillList;
    void** begin = *reinterpret_cast<void***>(list + 4);
    void** end = *reinterpret_cast<void***>(list + 8);
    const int n = begin ? static_cast<int>(end - begin) : 0;
    if (!family && g_config.hillField && mask >= 1 && mask <= n)
        if (void* tile = HillFieldTile(mask, x, y, begin[mask - 1]))
            return tile;
    const int variants = n / TileSets::kMasks;
    if (mask < 1 || mask > n || variants <= 1)
        return begin ? begin[mask - 1] : nullptr;  // as the engine does
    int wx = x, wy = y;
    Wrap(wx, wy);
    const TileSets::Range range = TileSets::SetRange(variants, g_sets, SquareSet(x, y));
    const int v = range.lo + RandAfter(TileSets::Seed(wx, wy) ^ (family ? 0x5EA7u : 0x4111u)) % range.size;
    ++(family ? g_peakVariants : g_hillVariants)[v < 16 ? v : 15];
    return begin[(mask - 1) + TileSets::kMasks * v];
}

// A coast square's variant index for the transition atlas (kind 0) or the wave model (1), folded into the art there is.
unsigned g_folded[2] = {};
extern "C" uint32_t g_tileSetsCoastClassFn = 0;  // FUN_004788e0's address, called by the two thunks below
extern "C" int __stdcall TileSetsCycle(int kind, int variant, int landMask)
{
    const int v = TileSets::CycleIndex(variant, landMask, kind ? g_config.coastWaveModels : g_config.coastAtlasCells);
    if (v != variant)
        ++g_folded[kind ? 1 : 0];
    return v;
}

extern "C" __declspec(naked) void TileSetsAtlasThunk()
{
    __asm {
        push edx  // the land mask, kept
        call dword ptr [g_tileSetsCoastClassFn]  // the engine's index (ECX, EAX, EDX as the caller set them)
        pop edx
        push edx  // land mask
        push eax  // variant
        push 0    // atlas
        call TileSetsCycle
        ret
    }
}

extern "C" __declspec(naked) void TileSetsWaveThunk()
{
    __asm {
        push edx
        call dword ptr [g_tileSetsCoastClassFn]
        pop edx
        push edx
        push eax
        push 1  // wave model
        call TileSetsCycle
        ret
    }
}

extern "C" __declspec(naked) void TileSetsPickerThunk()
{
    __asm {
        mov edx, esp
        push esi                  // the list, when the caller is FUN_00478180
        push dword ptr [edx]      // return address
        push dword ptr [edx + 8]  // hi
        push dword ptr [edx + 4]  // lo
        push eax                  // y
        push ecx                  // x
        call TileSetsPicker
        ret                       // the caller pops lo and hi
    }
}

extern "C" __declspec(naked) void TileSetsHillThunk()
{
    __asm {
        push ebx  // y
        push ebp  // x
        push eax  // mask
        push 0    // hills
        call TileSetsTile
        mov edx, eax
        ret
    }
}

extern "C" __declspec(naked) void TileSetsPeakThunk()
{
    __asm {
        push eax
        push ecx
        push edx
        push ebx  // y
        push ebp  // x
        push eax  // mask
        push 1    // peaks
        call TileSetsTile
        mov esi, eax
        pop edx
        pop ecx
        pop eax
        ret
    }
}

void ApplyTileSets()
{
    if (!g_config.tileSetsPatch && !g_config.hillField)
        return;
    g_sets = (std::max)(1, (std::min)(TileSets::kMaxSets, g_config.tileSets));
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = exe;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    for (auto& c : name)
        c = static_cast<wchar_t>(towlower(c));
    if (name != L"civ4beyondsword.exe")
    {
        Log::Write("terrain tile sets: not applied: %ls isn't Civ4BeyondSword.exe", name.c_str());
        return;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (reinterpret_cast<uint32_t>(base) != 0x400000)
    {
        Log::Write("terrain tile sets: not applied: the exe is loaded at %p, not 0x400000", base);
        return;
    }
    g_base = reinterpret_cast<uint32_t>(base);
    g_tileSetsCoastClassFn = g_base + TileSets::kCoastClassFn;
    const auto sites = TileSets::Sites(g_base, reinterpret_cast<uint32_t>(&TileSetsPickerThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsHillThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsPeakThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsAtlasThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsWaveThunk));
    int written = 0;
    const std::string why = TerrainPatch::ApplySites(base, nt->OptionalHeader.SizeOfImage, sites,
                                                     [&](uint32_t rva, const uint8_t* bytes, size_t count) {
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
        Log::Write("terrain tile sets: not applied: %s (exe timestamp 0x%08lx)", why.c_str(),
                   static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
    else
        Log::Write("terrain tile sets: %d of %u code sites written (picker, hill and peak lookups, coast atlas and wave "
                   "models); %d set%s; coast art per class: atlas cells %d,%d,%d,%d, wave models %d,%d,%d,%d",
                   written, static_cast<unsigned>(sites.size()), g_sets, g_sets == 1 ? "" : "s",
                   g_config.coastAtlasCells[0], g_config.coastAtlasCells[1], g_config.coastAtlasCells[2],
                   g_config.coastAtlasCells[3], g_config.coastWaveModels[0], g_config.coastWaveModels[1],
                   g_config.coastWaveModels[2], g_config.coastWaveModels[3]);
}

// [terrain tiles] LandBlend1..8 (terrain_sets.h): the engine's blend pick, then on a square with no water corner a land
// entry for terrains that have them.
extern "C" uint32_t g_blendPickFn = 0;  // FUN_00479920, called by the thunk below
namespace
{
std::vector<TileSets::LandBlend> g_landBlends;
std::vector<int> g_landBlendFor;  // by TerrainTypes: index into g_landBlends, or -1
bool g_landTableBuilt = false;
std::vector<unsigned> g_landPicks;  // by g_landBlends index: entries replaced on land squares

void BuildLandTable()
{
    g_landTableBuilt = true;
    if (!g_dll.Resolve() || !g_dll.isWater)
    {
        Log::Write("terrain land blends: the game DLL lacks the plot exports; the engine's picks stand");
        return;
    }
    void* gc = g_dll.getInstance();
    const int n = g_dll.numTerrains(gc, nullptr);
    g_landBlendFor.assign(n > 0 ? n : 0, -1);
    std::string found, missing;
    for (size_t k = 0; k < g_landBlends.size(); ++k)
    {
        int at = -1;
        for (int t = 0; t < n && at < 0; ++t)
        {
            std::string name = g_dll.type(g_dll.terrainInfo(gc, nullptr, t), nullptr);
            for (char& c : name)
                c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (name == g_landBlends[k].terrain)
                at = t;
        }
        if (at >= 0)
            g_landBlendFor[at] = static_cast<int>(k);
        (at >= 0 ? found : missing) += (at >= 0 ? (found.empty() ? "" : ", ") : (missing.empty() ? "" : ", ")) +
                                       g_landBlends[k].terrain;
    }
    Log::Write("terrain land blends: %s%s%s", found.empty() ? "no terrain found" : ("for " + found).c_str(),
               missing.empty() ? "" : "; not terrains of this game: ", missing.c_str());
}
}  // namespace

namespace
{
TileSets::BlendSquare g_blendSquare;
unsigned g_mixedSquares = 0;    // mixed-land coast squares given the full-mask pick
unsigned g_overlaySquares = 0;  // coast squares where an overlay (peaks) covering part of the land took the full mask
}  // namespace

// The engine's blend pick (FUN_00479920) called again, for another mask: EAX = terrain, EDI = where it writes its
// list index (a scratch here; the hook only replaces the tile and rotation).
extern "C" __declspec(naked) void __stdcall CallBlendPick(int terrain, int mask, int* tile, int* rotation, int x, int y)
{
    __asm {
        push ebp
        mov ebp, esp
        push edi
        sub esp, 4
        mov edi, esp                 // the index, discarded
        push dword ptr [ebp + 0x1c]  // y
        push dword ptr [ebp + 0x18]  // x
        push dword ptr [ebp + 0x14]  // &rotation
        push dword ptr [ebp + 0x10]  // &tile
        push dword ptr [ebp + 0x0c]  // mask
        mov eax, [ebp + 0x08]        // terrain
        call dword ptr [g_blendPickFn]  // pops its five
        add esp, 4
        pop edi
        pop ebp
        ret 0x18
    }
}

extern "C" void __stdcall LandBlendFix(int terrain, int mask, int* tile, int* rotation, int x, int y)
{
    if (!g_landTableBuilt)
        BuildLandTable();
    if (terrain < 0 || terrain >= static_cast<int>(g_landBlendFor.size()) || !g_dll.ok)
        return;
    // The square's corner plots as the heightmap and the layers find them (TL, TR, BR, BL); one off the map counts as
    // water, as the heightmap counts it.
    void* map = g_dll.Map();
    const int top = g_dll.height(map, nullptr) - y;
    const int at[4][2] = {{x - 1, top}, {x, top}, {x, top - 1}, {x - 1, top - 1}};
    TileSets::BlendCorners corners;
    for (int k = 0; k < 4; ++k)
    {
        void* plot = g_dll.plot(map, nullptr, at[k][0], at[k][1]);
        corners.water[k] = !plot || g_dll.isWater(plot, nullptr);
        corners.terrain[k] = plot ? g_dll.terrainType(plot, nullptr) : -1;
    }
    const int own = g_landBlendFor[terrain];
    int fullMask = 0;
    switch (g_blendSquare.Next(x, y, corners, terrain, mask, own >= 0, fullMask))
    {
    case TileSets::BlendAction::Engine:
        return;
    case TileSets::BlendAction::FullMask:
        CallBlendPick(terrain, fullMask, tile, rotation, x, y);
        ++(corners.terrain[0] == terrain || corners.terrain[1] == terrain || corners.terrain[2] == terrain ||
                   corners.terrain[3] == terrain
               ? g_mixedSquares
               : g_overlaySquares);
        return;
    case TileSets::BlendAction::Land:
        break;
    }
    const auto& choices = g_landBlends[own].masks[mask];
    int wx = x, wy = y;
    Wrap(wx, wy);
    const auto& pick = choices[RandAfter(TileSets::Seed(wx, wy) ^ 0x1A4Du) % choices.size()];
    *tile = pick.first;
    *rotation = pick.second;
    ++g_landPicks[own];
}

extern "C" __declspec(naked) void LandBlendThunk()
{
    __asm {
        push eax                     // the terrain
        push dword ptr [esp + 0x18]  // y
        push dword ptr [esp + 0x18]  // x
        push dword ptr [esp + 0x18]  // &rotation
        push dword ptr [esp + 0x18]  // &tile
        push dword ptr [esp + 0x18]  // mask
        mov eax, [esp + 0x14]        // the terrain again, for the engine's pick
        call dword ptr [g_blendPickFn]  // pops its five
        pop eax
        push dword ptr [esp + 0x14]  // y
        push dword ptr [esp + 0x14]  // x
        push dword ptr [esp + 0x14]  // &rotation
        push dword ptr [esp + 0x14]  // &tile
        push dword ptr [esp + 0x14]  // mask
        push eax                     // terrain
        call LandBlendFix
        ret 0x14
    }
}

void ApplyLandBlends()
{
    // LandBlend1..8 (the full form) first: they win over the short form's standard layout for the same terrain.
    for (const std::wstring& w : g_config.landBlends)
    {
        if (w.empty())
            continue;
        TileSets::LandBlend blend;
        std::string why;
        if (TileSets::ParseLandBlend(std::string(w.begin(), w.end()), blend, why))
            g_landBlends.push_back(blend);
        else
            Log::Write("terrain land blends: \"%ls\" ignored: %s", w.c_str(), why.c_str());
    }
    const std::wstring& shortForm = g_config.landBlendTerrains;
    for (const std::string& name : TileSets::LandBlendNames(std::string(shortForm.begin(), shortForm.end())))
    {
        bool listed = false;
        for (const auto& b : g_landBlends)
            listed = listed || b.terrain == name;
        if (!listed)
            g_landBlends.push_back(TileSets::StandardLandBlend(name));
    }
    if (g_landBlends.empty())
        return;
    g_landPicks.assign(g_landBlends.size(), 0);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = exe;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    for (auto& c : name)
        c = static_cast<wchar_t>(towlower(c));
    if (name != L"civ4beyondsword.exe")
    {
        Log::Write("terrain land blends: not applied: %ls isn't Civ4BeyondSword.exe", name.c_str());
        return;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (reinterpret_cast<uint32_t>(base) != 0x400000)
    {
        Log::Write("terrain land blends: not applied: the exe is loaded at %p, not 0x400000", base);
        return;
    }
    g_base = reinterpret_cast<uint32_t>(base);
    g_blendPickFn = g_base + TileSets::kBlendPick;
    const auto sites = TileSets::LandBlendSites(g_base, reinterpret_cast<uint32_t>(&LandBlendThunk));
    int written = 0;
    const std::string why = TerrainPatch::ApplySites(base, nt->OptionalHeader.SizeOfImage, sites,
                                                     [&](uint32_t rva, const uint8_t* bytes, size_t count) {
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
        Log::Write("terrain land blends: not applied: %s (exe timestamp 0x%08lx)", why.c_str(),
                   static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
    else
        Log::Write("terrain land blends: %d of %u code sites written; %u terrain%s with land entries", written,
                   static_cast<unsigned>(sites.size()), static_cast<unsigned>(g_landBlends.size()),
                   g_landBlends.size() == 1 ? "" : "s");
}

std::string LandBlendsSummary()
{
    static unsigned logged = 0;
    unsigned total = 0;
    for (unsigned n : g_landPicks)
        total += n;
    total += g_mixedSquares + g_overlaySquares;
    if (total == logged)
        return "";
    logged = total;
    std::string s = "terrain land blends: land entries given:";
    for (size_t k = 0; k < g_landBlends.size(); ++k)
        s += " " + g_landBlends[k].terrain + "=" + std::to_string(g_landPicks[k]);
    s += "; coast squares of two or more land terrains painted by their lowest land layer for the whole land: " +
         std::to_string(g_mixedSquares) + "; coast squares where a hill or peak layer covering part of the land "
         "painted the whole: " + std::to_string(g_overlaySquares);
    return s;
}

std::string TileSetsSummary()
{
    unsigned total = 0;
    for (unsigned p : g_picks)
        total += p;
    for (int v = 0; v < 16; ++v)
        total += g_hillVariants[v] + g_peakVariants[v];
    total += g_folded[0] + g_folded[1];
    total += g_hillCut + g_hillStock + g_hillRefits + g_hillUpdates;
    if (total == g_logged)
        return "";
    g_logged = total;
    // Numbered from 1, as the INI numbers the sets (Set1, Set2...) and the files (hill1_1-15 is variant 1).
    std::string s = "terrain tile sets: tiles picked by set:";
    for (int k = 0; k < g_sets; ++k)
        s += " Set" + std::to_string(k + 1) + "=" + std::to_string(g_picks[k]);
    auto variants = [&](const char* what, const unsigned* counts) {
        s += std::string("; ") + what + " variants:";
        for (int v = 0; v < 16; ++v)
            if (counts[v])
                s += " " + std::to_string(v + 1) + "=" + std::to_string(counts[v]);
    };
    variants("hill", g_hillVariants);
    variants("peak", g_peakVariants);
    s += "; by list (";
    for (int k = 0; k < g_sets; ++k)
        s += (k ? "/Set" : "Set") + std::to_string(k + 1);
    s += "):";
    for (int l = 0; l < kLists; ++l)
    {
        unsigned n = 0;
        for (int k = 0; k < g_sets; ++k)
            n += g_listPicks[l][k];
        if (!n)
            continue;
        s += std::string(" ") + kListNames[l];
        for (int k = 0; k < g_sets; ++k)
            s += (k ? "/" : " ") + std::to_string(g_listPicks[l][k]);
        s += ",";
    }
    if (s.back() == ',')
        s.pop_back();
    if (g_folded[0] || g_folded[1])
        s += "; coast variants folded into the art: atlas " + std::to_string(g_folded[0]) + ", wave models " +
             std::to_string(g_folded[1]);
    if (g_hillCut || g_hillStock)
        s += "; hill field: " + std::to_string(g_hillCut) + " tiles cut, " + std::to_string(g_hillStock) +
             " stock, " + std::to_string(g_hillRefits) + " fits, " + std::to_string(g_hillUpdates) + " local updates";
    if (!g_setFlats.empty() && !g_setFlatsLogged)
    {
        g_setFlatsLogged = g_setFlats.size() >= 12;
        s += "; flat squares not in Set1 (corner plots x,y to x+1,y+1):";
        for (const Square& q : g_setFlats)
            s += " (" + std::to_string(q.x0) + "," + std::to_string(q.y0) + " Set" + std::to_string(q.set + 1) + ")";
    }
    return s;
}
