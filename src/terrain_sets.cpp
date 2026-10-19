// [terrain tiles] Patch, Sets, Set1..Set8: heightmap tile sets by terrain, and hill/peak variants (terrain_sets.h). The
// INI and the log number sets from 1 (Set1 = set 0 here); everything inside is 0-based.
//
// Three hooks in the running Civ4BeyondSword.exe, written only if every site holds BtS's own bytes:
//   - the variant picker jumps to PickerThunk (ECX = x, EAX = y, [ESP+4] = lo, [ESP+8] = hi, caller cleans);
//   - the hill and peak lookups call HillThunk / PeakThunk (EAX = corner mask, EBP = x, EBX = y), which return the
//     tile in EDX / ESI.
// Map, plot and terrain come from the game DLL's exports (thiscall, called as fastcall with an unused EDX).
#include "common.h"
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
    MemberBool wrapX = nullptr, wrapY = nullptr, isWater = nullptr;
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
    if (!g_config.tileSetsPatch)
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

std::string TileSetsSummary()
{
    unsigned total = 0;
    for (unsigned p : g_picks)
        total += p;
    for (int v = 0; v < 16; ++v)
        total += g_hillVariants[v] + g_peakVariants[v];
    total += g_folded[0] + g_folded[1];
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
    if (!g_setFlats.empty() && !g_setFlatsLogged)
    {
        g_setFlatsLogged = g_setFlats.size() >= 12;
        s += "; flat squares not in Set1 (corner plots x,y to x+1,y+1):";
        for (const Square& q : g_setFlats)
            s += " (" + std::to_string(q.x0) + "," + std::to_string(q.y0) + " Set" + std::to_string(q.set + 1) + ")";
    }
    return s;
}
