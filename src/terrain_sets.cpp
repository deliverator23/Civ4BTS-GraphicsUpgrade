// [terrain tiles] TilePicker, TileGroups, TileGroup1..8: heightmap tile groups by terrain, and hill/peak variants
// (terrain_sets.h). A tile group is a "set" here: the INI and the log number them from 1 (TileGroup1 = set 0 here);
// everything inside is 0-based.
//
// Three hooks in the running Civ4BeyondSword.exe, written only if every site holds BtS's own bytes:
//   - the variant picker jumps to PickerThunk (ECX = x, EAX = y, [ESP+4] = lo, [ESP+8] = hi, caller cleans);
//   - the hill and peak lookups call HillThunk / PeakThunk (EAX = corner mask, EBP = x, EBX = y), which return the
//     tile in EDX / ESI.
// Map, plot and terrain come from the game DLL's exports (thiscall, called as fastcall with an unused EDX).
//
// [terrain tiles] HillStamps (experimental, hill_field.h): the hill lookup hands back a tile cut from one map-wide
// field of hill stamps instead of hill1_<mask>.
//
// [terrain tiles] JigsawPieces1..8 (experimental, jigsaw.h): the base tile stamp (FUN_00478a90) is entered through
// JigsawBaseThunk, which stamps a square touching a set's plots with those corners as the set says (water, land or as
// they are), then adds the sets' fields.
//
// [terrain tiles] PlotStamps1..8 (experimental, plot_stamps.h): through the same thunk, the stamps on a square's corner
// plots are added after its base tile too (on top of the jigsaw pieces' fields, where both reach a square).
#include "common.h"
#include "assets.h"
#include "jigsaw.h"
#include "hill_field.h"
#include "plot_stamps.h"
#include "river_layer.h"
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
    using MemberFloat = float(__fastcall*)(void* self, void* unused);
    GetInstance getInstance = nullptr;
    Member getMap = nullptr;
    MemberInt width = nullptr, height = nullptr, numTerrains = nullptr, terrainType = nullptr;
    MemberBool wrapX = nullptr, wrapY = nullptr, isWater = nullptr, isHills = nullptr, isPeak = nullptr;
    MemberBool isNOfRiver = nullptr, isWOfRiver = nullptr;  // optional: the river layer
    MemberArgs plot = nullptr;
    MemberArg terrainInfo = nullptr;
    MemberStr type = nullptr;
    // optional: plot stamps (features, and the landscape's grid step in world units)
    MemberInt featureType = nullptr, numFeatures = nullptr, activeLandscape = nullptr;
    MemberArg featureInfo = nullptr, landscapeInfo = nullptr;
    MemberFloat zScale = nullptr;
    // optional: MixedCoastBlend (each terrain's art: its layer order and blend sheet; the size of a plot)
    using MemberNamed = void*(__fastcall*)(void* self, void* unused, const char* name);
    using MemberFloatNamed = float(__fastcall*)(void* self, void* unused, const char* name);
    MemberStr artDefineTag = nullptr, baseTexture = nullptr, gridTexture = nullptr;
    GetInstance artFileMgr = nullptr;
    MemberNamed terrainArt = nullptr;
    MemberInt layerOrder = nullptr;
    MemberFloatNamed defineFloat = nullptr;
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
        isPeak = reinterpret_cast<MemberBool>(get("?isPeak@CvPlot@@QBE_NXZ"));    // optional: jigsaw sets
        isNOfRiver = reinterpret_cast<MemberBool>(get("?isNOfRiver@CvPlot@@QBE_NXZ"));
        isWOfRiver = reinterpret_cast<MemberBool>(get("?isWOfRiver@CvPlot@@QBE_NXZ"));
        numTerrains = reinterpret_cast<MemberInt>(get("?getNumTerrainInfos@CvGlobals@@QAEHXZ"));
        terrainInfo = reinterpret_cast<MemberArg>(get("?getTerrainInfo@CvGlobals@@QAEAAVCvTerrainInfo@@W4TerrainTypes@@@Z"));
        type = reinterpret_cast<MemberStr>(get("?getType@CvInfoBase@@QBEPBDXZ"));
        featureType = reinterpret_cast<MemberInt>(get("?getFeatureType@CvPlot@@QBE?AW4FeatureTypes@@XZ"));
        numFeatures = reinterpret_cast<MemberInt>(get("?getNumFeatureInfos@CvGlobals@@QAEHXZ"));
        featureInfo = reinterpret_cast<MemberArg>(get("?getFeatureInfo@CvGlobals@@QAEAAVCvFeatureInfo@@W4FeatureTypes@@@Z"));
        activeLandscape = reinterpret_cast<MemberInt>(get("?getActiveLandscapeID@CvGlobals@@QAEHXZ"));
        landscapeInfo = reinterpret_cast<MemberArg>(get("?getLandscapeInfo@CvGlobals@@QAEAAVCvLandscapeInfo@@H@Z"));
        zScale = reinterpret_cast<MemberFloat>(get("?getZScale@CvLandscapeInfo@@QBEMXZ"));
        artDefineTag = reinterpret_cast<MemberStr>(get("?getArtDefineTag@CvTerrainInfo@@QBEPBDXZ"));
        artFileMgr = reinterpret_cast<GetInstance>(get("?GetInstance@CvArtFileMgr@@SAAAV1@XZ"));
        terrainArt = reinterpret_cast<MemberNamed>(get("?getTerrainArtInfo@CvArtFileMgr@@QBEPAVCvArtInfoTerrain@@PBD@Z"));
        layerOrder = reinterpret_cast<MemberInt>(get("?getLayerOrder@CvArtInfoTerrain@@QAEHXZ"));
        baseTexture = reinterpret_cast<MemberStr>(get("?getBaseTexture@CvArtInfoTerrain@@QAEPBDXZ"));
        gridTexture = reinterpret_cast<MemberStr>(get("?getGridTexture@CvArtInfoTerrain@@QAEPBDXZ"));
        defineFloat = reinterpret_cast<MemberFloatNamed>(get("?getDefineFLOAT@CvGlobals@@QBEMPBD@Z"));
        ok = getInstance && getMap && width && height && wrapX && wrapY && plot && terrainType && numTerrains &&
             terrainInfo && type;
        if (!ok)
            Log::Write("terrain tile groups: the game DLL lacks the map/plot/terrain exports; every square uses "
                       "TileGroup1");
        return ok;
    }
    void* Map() { return getMap(getInstance(), nullptr); }
};
GameDll g_dll;

uint32_t g_base = 0;
const Exe::Layout* g_exe = &Exe::kSteam;  // the running build's, once a patch has found it (Exe::Identify)
int g_sets = 1;
const int kUnlisted = -2;
// by TerrainTypes: the set (0-based; the INI's TileGroup1 is 0) it's listed in, or kUnlisted
std::vector<int> g_terrainSet;
bool g_tableBuilt = false;
unsigned g_picks[TileSets::kMaxSets] = {}, g_hillVariants[16] = {}, g_peakVariants[16] = {};
unsigned g_logged = 0;

// Which list a pick was for: FUN_00478180 (listCaller) gets it in ESI; the other callers by where they return.
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
    const Exe::Layout& x = *g_exe;
    const uint32_t rva = returnAddress - g_base;
    if (rva == x.listCaller)  // FUN_00478180: the list object in ESI (lists in the loader's order)
    {
        const List byLoad[8] = {kOcean, kFlats, kOther, kOther, kCoast1, kCoast3, kCoast7, kCoast5};
        for (int k = 0; k < 8; ++k)
            if (esi == x.lists[k])
                return byLoad[k];
        return kOther;
    }
    if (rva == x.coast1Caller)  // FUN_00478a90's one-land-corner coasts
        return kCoast1;
    if (rva >= x.coastClassFn && rva < x.coastClassFnEnd)  // FUN_004788e0: the coast class index the waves and atlas use
        return kCoastClass;
    if (rva >= x.maskPicker && rva < x.maskPickerEnd)  // FUN_00478f50: the coast scale mask
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
            // TileGroup<s + 1>: terrain types separated by commas, semicolons or spaces, any case
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
                listed += (listed.empty() ? "" : ", ") + upper + " TileGroup" + std::to_string(s + 1);
            }
        }
    }
    Log::Write("terrain tile groups: %d groups; %s; other land TileGroup1, other water no vote", g_sets,
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
        if (votes[k] == kUnlisted)  // land listed nowhere: TileGroup1; water listed nowhere doesn't vote
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

// [terrain tiles] HillStamps: the map's hills as one field of stamps (hill_field.h). The engine stamps hills square by
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
            Log::Write("terrain tiles: HillStamps off: the game DLL lacks CvPlot::isHills");
        g_hillLoggedOff = true;
        return nullptr;
    }
    if (!g_hillStampsSet)
    {
        g_hillStampsSet = true;
        g_hillField.SetStamps(HillField::SyntheticStamps());
        Log::Write("terrain tiles: HillStamps on, with %u made-up stamps (synthetic_*; gain %.2f)",
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

// [terrain tiles] JigsawPieces1..8: each set's plots (flat land and water of its terrains) as one field of its jigsaw
// pieces (jigsaw.h). The base stamp runs for every square, row by row, so as with the hills a call before the last one
// starts a new pass: the plots are read again and the placements brought up to date first.
struct JigsawRun
{
    Jigsaw::Field field;
    int base = Jigsaw::kKeep;
    std::string name;  // "JigsawPieces1"...
};
std::vector<JigsawRun> g_jigsaws;  // the sets that run
std::vector<int> g_jigsawOf;       // by TerrainTypes: the set (in g_jigsaws) its plots are in, or -1
HillField::Map g_jigsawLand;       // the map's land plots as read (in .hills)
bool g_jigsawsRead = false, g_jigsawPending = false;
int g_jigsawLastX = -1, g_jigsawLastY = -1, g_jigsawX = 0, g_jigsawY = 0;
unsigned g_jigsawSquares = 0, g_jigsawAdded = 0, g_jigsawMisfit = 0, g_jigsawDisagree = 0, g_jigsawFits = 0,
         g_jigsawUpdates = 0;
std::vector<float> g_jigsawTile;
std::vector<Jigsaw::Combine> g_jigsawSum;

bool AnyJigsaw()
{
    for (const auto& j : g_config.jigsaw)
        if (!j.terrains.empty())
            return true;
    return false;
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w)
        s += static_cast<char>(c < 128 ? c : '?');
    return s;
}

// A file of a set's pieces: in its folder, which is a full path or one under Assets (the mod's first, loose or packed).
bool ReadPiecesFile(const std::wstring& folder, const std::string& file, std::vector<uint8_t>& out, std::wstring& from)
{
    std::wstring path = folder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    for (char c : file)
        path += static_cast<wchar_t>(c == '/' ? '\\' : c);
    const bool full = (path.size() > 1 && path[1] == L':') || path.rfind(L"\\\\", 0) == 0;
    from = path;
    return full ? ReadFileBytes(path, out) : ReadAsset(path, out, from);
}

// Every set's terrains and pieces, once; false if none can run (each one's trouble logged).
bool JigsawsReady()
{
    if (g_jigsawsRead)
        return !g_jigsaws.empty();
    g_jigsawsRead = true;
    if (!AnyJigsaw())
        return false;
    if (!g_dll.Resolve() || !g_dll.isWater || !g_dll.isHills || !g_dll.isPeak)
    {
        Log::Write("terrain tiles: jigsaw pieces off: the game DLL lacks CvPlot::isWater, isHills or isPeak");
        return false;
    }
    void* gc = g_dll.getInstance();
    const int count = g_dll.numTerrains(gc, nullptr);
    std::vector<std::string> names(count > 0 ? count : 0);
    for (int t = 0; t < count; ++t)
    {
        const char* name = g_dll.type(g_dll.terrainInfo(gc, nullptr, t), nullptr);
        for (const char* c = name ? name : ""; *c; ++c)
            names[t] += static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    }
    g_jigsawOf.assign(names.size(), -1);
    for (int s = 0; s < 8; ++s)
    {
        const Config::JigsawSet& cfg = g_config.jigsaw[s];
        if (cfg.terrains.empty())
            continue;
        const std::string set = "JigsawPieces" + std::to_string(s + 1);
        std::string list = " ";
        for (wchar_t c : cfg.terrains)
            list += (c == ',' || c == ';' || c == '\t') ? ' ' : static_cast<char>(towupper(c));
        list += " ";
        std::vector<int> terrains;
        std::string listed;
        for (int t = 0; t < static_cast<int>(names.size()); ++t)
            if (!names[t].empty() && list.find(" " + names[t] + " ") != std::string::npos)
            {
                if (g_jigsawOf[t] >= 0)
                {
                    Log::Write("terrain tiles: %s: %s is already in an earlier JigsawPieces<n>Terrains", set.c_str(),
                               names[t].c_str());
                    continue;
                }
                terrains.push_back(t);
                listed += (listed.empty() ? "" : ", ") + names[t];
            }
        if (terrains.empty())
        {
            Log::Write("terrain tiles: %s off: no terrain of %ls to cover", set.c_str(), cfg.terrains.c_str());
            continue;
        }
        if (cfg.pieces.empty())
        {
            Log::Write("terrain tiles: %s off: no %sFolder set", set.c_str(), set.c_str());
            continue;
        }
        std::vector<uint8_t> bytes;
        std::wstring from;
        Jigsaw::PiecesXml xml;
        std::string why = "can't be read";
        if (!ReadPiecesFile(cfg.pieces, "pieces.xml", bytes, from) ||
            !Jigsaw::ParsePiecesXml(std::string(bytes.begin(), bytes.end()), xml, why))
        {
            Log::Write("terrain tiles: %s off: %ls: %s", set.c_str(), from.c_str(), why.c_str());
            continue;
        }
        std::vector<Jigsaw::Piece> pieces;
        int sizes[17] = {}, left = 0;
        for (const Jigsaw::PieceFile& pf : xml.pieces)
        {
            std::vector<uint8_t> dds;
            std::wstring at;
            std::vector<uint16_t> pixels;
            int w = 0, h = 0;
            Jigsaw::Piece p;
            std::string bad = "can't be read";
            if (!ReadPiecesFile(cfg.pieces, pf.file, dds, at) || !Jigsaw::DecodeDds(dds.data(), dds.size(), w, h, pixels, bad) ||
                !Jigsaw::MakePiece(pf.file, pf.plots, w, h, std::move(pixels), p, bad))
            {
                if (left++ < 5)
                    Log::Write("terrain tiles: %s: piece %ls left out: %s", set.c_str(), at.c_str(), bad.c_str());
                continue;
            }
            ++sizes[(std::min)(16, p.Size())];
            pieces.push_back(std::move(p));
        }
        const float height = cfg.hasHeight ? cfg.height : xml.heightScale;
        JigsawRun run;
        run.base = cfg.base;
        run.name = set;
        const unsigned read = static_cast<unsigned>(pieces.size());
        run.field.SetPieces(std::move(pieces), height / Jigsaw::kValuePerLevel);
        if (!run.field.Usable())
        {
            Log::Write("terrain tiles: %s off: no one-plot piece read from %ls (each plot needs one when nothing bigger "
                       "fits)",
                       set.c_str(), from.c_str());
            continue;
        }
        static const char* const kBases[3] = {"as they are", "as if its corners were water", "as if its corners were land"};
        Log::Write("terrain tiles: %s: %s (flat land and water); %u pieces (%d one-plot, %d two, %d three, %d four, %d "
                   "more) from %ls%s; height %.2f grid units a level (%s); base tiles %s",
                   set.c_str(), listed.c_str(), read, sizes[1], sizes[2], sizes[3], sizes[4],
                   static_cast<int>(read) - sizes[1] - sizes[2] - sizes[3] - sizes[4], from.c_str(),
                   left ? (", " + std::to_string(left) + " left out").c_str() : "", height,
                   cfg.hasHeight ? "the INI's" : xml.hasHeight ? "pieces.xml's" : "default",
                   kBases[(std::min)(2, (std::max)(0, cfg.base))]);
        for (int t : terrains)
            g_jigsawOf[t] = static_cast<int>(g_jigsaws.size());
        g_jigsaws.push_back(std::move(run));
    }
    return !g_jigsaws.empty();
}

void JigsawSync()
{
    void* map = g_dll.Map();
    HillField::Map m;
    m.width = g_dll.width(map, nullptr);
    m.height = g_dll.height(map, nullptr);
    m.wrapX = g_dll.wrapX(map, nullptr);
    m.wrapY = g_dll.wrapY(map, nullptr);
    if (m.width <= 0 || m.height <= 0 || m.width > 1024 || m.height > 1024)
        return;
    m.hills.assign(static_cast<size_t>(m.width) * m.height, 0);
    std::vector<HillField::Map> sets(g_jigsaws.size(), m);
    std::vector<int> plots(g_jigsaws.size(), 0);
    g_jigsawLand = m;
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
        {
            void* plot = g_dll.plot(map, nullptr, x, y);
            if (!plot)
                continue;
            const size_t i = static_cast<size_t>(y) * m.width + x;
            g_jigsawLand.hills[i] = g_dll.isWater(plot, nullptr) ? 0 : 1;
            if (g_dll.isHills(plot, nullptr) || g_dll.isPeak(plot, nullptr))
                continue;
            const int t = g_dll.terrainType(plot, nullptr);
            const int s = t >= 0 && t < static_cast<int>(g_jigsawOf.size()) ? g_jigsawOf[t] : -1;
            if (s >= 0)
                sets[s].hills[i] = 1, ++plots[s];
        }
    for (size_t s = 0; s < g_jigsaws.size(); ++s)
    {
        const int how = g_jigsaws[s].field.Update(sets[s]);
        if (how == 2)
        {
            ++g_jigsawFits;
            int sizes[17] = {};
            for (const auto& p : g_jigsaws[s].field.Placements())
                ++sizes[(std::min)(16, static_cast<int>(p.frame.size()))];
            Log::Write("terrain tiles: %s placed on a %dx%d map (%d plots): %d four-plot, %d three-plot, %d two-plot and "
                       "%d one-plot pieces",
                       g_jigsaws[s].name.c_str(), m.width, m.height, plots[s], sizes[4], sizes[3], sizes[2], sizes[1]);
        }
        else if (how == 1)
            ++g_jigsawUpdates;
    }
}

// [terrain tiles] PlotStamps1..8: a stamp on each plot of a set's terrains (plot_stamps.h), added with the jigsaw
// pieces after the base tile stamp. A square's stamps come from its corner plots as they are when it's stamped.
struct PlotStampRun
{
    std::string name;                   // "PlotStamps1"...
    std::vector<Jigsaw::Piece> pieces;  // its one-plot pieces
    std::vector<int> features;          // PlotStampsNFeatures as feature types, in order (-1: no such feature)
    float units = 0;                    // grid units a pixel value
    std::vector<int> cells;             // by piece: its painting's atlas cell + 1, 255 for none; empty: no paintings
    int layer = 0;                      // the number of the layer its paintings go on: its own, or kPeakLayer
    bool flatten = false;               // PlotStampsNFlatten: its pieces are weights
};
const int kPeakLayer = 9;                // PlotStamps<n>OnFeatures: their paintings all go on the peak overlay's draw
std::vector<PlotStampRun> g_plotStamps;  // the sets that run
std::vector<int> g_plotStampOnFeature;   // by FeatureTypes: the set (in g_plotStamps) its plots are in, or -1
bool g_flattenHooked = false;            // PlotStamps<n>Flatten: the peak stamp's calls are wrapped
unsigned g_flattenSquares = 0;
bool g_flattenEdgeLogged = false;
// [terrain tiles] PlotStamps<n>Textures: the paintings, the sheets of the layers they go on, and each plot's cell.
struct StampPaintings
{
    std::vector<std::vector<uint8_t>> dds;  // a file a cell
    std::vector<PlotStampsSheet> sheets;
    PlotStamps::ColourFile first;           // they all have its size and format
    std::vector<uint8_t> cells;             // two bytes a plot: as PlotStampsCells
    int width = 0, height = 0;
    bool wrapX = false, wrapY = false, allLayers = false;
    float toPlot[3] = {};
    float detail = 0;                       // [debug] PlotStampsTest: its layer's
    bool over = false, multiply = false;
    unsigned version = 0;
    bool testTried = false;
} g_paintings;
std::vector<int> g_plotStampOf;          // by TerrainTypes: the set (in g_plotStamps) its plots are in, or -1
bool g_plotStampsRead = false;
unsigned g_stampSquares = 0, g_stampClipped = 0, g_stampMisfit = 0;
std::vector<float> g_stampTile;

bool AnyPlotStamps()
{
    for (const auto& p : g_config.plotStamps)
        if (!p.terrains.empty() || !p.onFeatures.empty())
            return true;
    return false;
}

// An INI list's names (separated by commas, semicolons or spaces), upper case, in order.
std::vector<std::string> ListNames(const std::wstring& list)
{
    std::vector<std::string> out;
    std::string name;
    for (wchar_t c : list + L" ")
        if (c == ',' || c == ';' || c == ' ' || c == '\t')
        {
            if (!name.empty())
                out.push_back(name);
            name.clear();
        }
        else
            name += static_cast<char>(towupper(c));
    return out;
}

// The game's info types by index (terrains, features), upper case.
std::vector<std::string> InfoNames(GameDll::MemberInt count, GameDll::MemberArg info)
{
    void* gc = g_dll.getInstance();
    const int n = count(gc, nullptr);
    std::vector<std::string> names(n > 0 ? n : 0);
    for (int t = 0; t < n; ++t)
    {
        const char* name = g_dll.type(info(gc, nullptr, t), nullptr);
        for (const char* c = name ? name : ""; *c; ++c)
            names[t] += static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    }
    return names;
}

// A set's paintings (PlotStamps<n>Textures), into the atlas's next cells; the pieces without one stay with the Mesa's
// blend. The layer must have a blend sheet of its own: that's how a draw is known to be its layer (Shadows::StampFor).
// layerTerrains: the terrains whose layers they go on (the peak overlay's for a set by feature).
void ReadPaintings(PlotStampRun& run, const Config::PlotStampSet& cfg, const std::vector<int>& layerTerrains, bool over)
{
    const char* set = run.name.c_str();
    run.cells.assign(run.pieces.size(), 255);
    if (!g_dll.artDefineTag || !g_dll.artFileMgr || !g_dll.terrainArt || !g_dll.baseTexture || !g_dll.gridTexture)
    {
        Log::Write("terrain tiles: %s: no paintings: the game DLL lacks the terrain art exports", set);
        return;
    }
    void* gc = g_dll.getInstance();
    auto artOf = [&](int t) {
        const char* tag = g_dll.artDefineTag(g_dll.terrainInfo(gc, nullptr, t), nullptr);
        return tag ? g_dll.terrainArt(g_dll.artFileMgr(), nullptr, tag) : nullptr;
    };
    auto same = [](std::string a, std::string b) {
        for (std::string* s : {&a, &b})
            for (char& c : *s)
                c = c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return a == b;
    };
    std::vector<PlotStampsSheet> sheets;
    const int count = g_dll.numTerrains(gc, nullptr);
    if (layerTerrains.empty())
    {
        Log::Write("terrain tiles: %s: no paintings: this game has no TERRAIN_PEAK layer to put them on", set);
        return;
    }
    for (int t : layerTerrains)
    {
        void* art = artOf(t);
        const char* base = art ? g_dll.baseTexture(art, nullptr) : nullptr;
        const char* grid = art ? g_dll.gridTexture(art, nullptr) : nullptr;
        if (!base || !*base)
        {
            Log::Write("terrain tiles: %s: no paintings: terrain %d has no blend sheet", set, t);
            return;
        }
        for (int other = 0; other < count; ++other)
        {
            void* otherArt = other == t ? nullptr : artOf(other);
            const char* otherBase = otherArt ? g_dll.baseTexture(otherArt, nullptr) : nullptr;
            if (otherBase && same(base, otherBase) &&
                std::find(layerTerrains.begin(), layerTerrains.end(), other) == layerTerrains.end())
            {
                Log::Write("terrain tiles: %s: no paintings: %s shares its blend sheet %s with %s, so its layer can't be "
                           "told from that one's; it needs a sheet of its own", set,
                           g_dll.type(g_dll.terrainInfo(gc, nullptr, t), nullptr), base,
                           g_dll.type(g_dll.terrainInfo(gc, nullptr, other), nullptr));
                return;
            }
        }
        sheets.push_back({std::wstring(base, base + strlen(base)), false, run.layer, over, cfg.textureMultiply,
                          cfg.textureDetail});
        if (grid && *grid)
            sheets.push_back({std::wstring(grid, grid + strlen(grid)), true, run.layer, over, cfg.textureMultiply,
                              cfg.textureDetail});
    }
    int read = 0;
    for (size_t k = 0; k < run.pieces.size(); ++k)
    {
        std::string file = run.pieces[k].name;
        const size_t dot = file.rfind('.');
        file = file.substr(0, dot) + "_colour.dds";
        std::vector<uint8_t> dds;
        std::wstring from;
        PlotStamps::ColourFile colour;
        std::string why = "can't be read";
        if (!ReadPiecesFile(cfg.folder, file, dds, from) || !PlotStamps::ParseColourDds(dds, colour, why) ||
            (!g_paintings.dds.empty() &&
             (colour.size != g_paintings.first.size || colour.dxt5 != g_paintings.first.dxt5) &&
             (why = "not the size and format of the first", true)) ||
            (g_paintings.dds.size() >= 254 && (why = "too many", true)))
        {
            Log::Write("terrain tiles: %s: %s has no painting: %s: %s", set, run.pieces[k].name.c_str(), file.c_str(),
                       why.c_str());
            continue;
        }
        g_paintings.dds.push_back(std::move(dds));
        g_paintings.first.size = colour.size;
        g_paintings.first.dxt5 = colour.dxt5;
        run.cells[k] = static_cast<int>(g_paintings.dds.size());
        ++read;
    }
    if (read)
        g_paintings.sheets.insert(g_paintings.sheets.end(), sheets.begin(), sheets.end());
    Log::Write("terrain tiles: %s: %d of %u paintings, %dx%d %s", set, read, static_cast<unsigned>(run.pieces.size()),
               g_paintings.first.size, g_paintings.first.size, g_paintings.first.dxt5 ? "DXT5" : "A8R8G8B8");
}

// The plots' cells, for this map's size.
void PaintingsMap(int width, int height, bool wrapX, bool wrapY)
{
    if (g_paintings.width == width && g_paintings.height == height)
        return;
    g_paintings.width = width;
    g_paintings.height = height;
    g_paintings.wrapX = wrapX;
    g_paintings.wrapY = wrapY;
    g_paintings.cells.assign(static_cast<size_t>(width) * height * 2, 0);
    const float plot = g_dll.defineFloat ? g_dll.defineFloat(g_dll.getInstance(), nullptr, "PLOT_SIZE") : 180.0f;
    g_paintings.toPlot[0] = 1.0f / plot;
    g_paintings.toPlot[1] = width * 0.5f - 0.5f;  // plot centres evenly spaced round the middle of the map
    g_paintings.toPlot[2] = height * 0.5f - 0.5f;
    ++g_paintings.version;
}

// Every set's terrains, features and pieces, once; false if none can run (each one's trouble logged).
bool PlotStampsReady()
{
    if (g_plotStampsRead)
        return !g_plotStamps.empty();
    g_plotStampsRead = true;
    if (!AnyPlotStamps() || !g_dll.Resolve())
        return false;
    if (!g_dll.featureType || !g_dll.numFeatures || !g_dll.featureInfo)
    {
        Log::Write("terrain tiles: plot stamps off: the game DLL lacks CvPlot::getFeatureType or the feature infos");
        return false;
    }
    // World units a grid step: the active landscape's fZScale (BtS's and Dune Wars' 1.0).
    void* gc = g_dll.getInstance();
    float zScale = 1.0f;
    bool zRead = false;
    if (g_dll.activeLandscape && g_dll.landscapeInfo && g_dll.zScale)
        if (void* landscape = g_dll.landscapeInfo(gc, nullptr, g_dll.activeLandscape(gc, nullptr)))
        {
            const float z = g_dll.zScale(landscape, nullptr);
            if (z > 0.001f && z < 1000.0f)
                zScale = z, zRead = true;
        }
    const std::vector<std::string> terrainNames = InfoNames(g_dll.numTerrains, g_dll.terrainInfo);
    const std::vector<std::string> featureNames = InfoNames(g_dll.numFeatures, g_dll.featureInfo);
    auto find = [](const std::vector<std::string>& names, const std::string& name) {
        const auto it = std::find(names.begin(), names.end(), name);
        return it == names.end() ? -1 : static_cast<int>(it - names.begin());
    };
    g_plotStampOf.assign(terrainNames.size(), -1);
    g_plotStampOnFeature.assign(featureNames.size(), -1);
    for (int s = 0; s < 8; ++s)
    {
        const Config::PlotStampSet& cfg = g_config.plotStamps[s];
        if (cfg.terrains.empty() && cfg.onFeatures.empty())
            continue;
        PlotStampRun run;
        run.name = "PlotStamps" + std::to_string(s + 1);
        const char* set = run.name.c_str();
        run.flatten = cfg.flatten;
        if (cfg.flatten && !g_flattenHooked)
        {
            // Its pieces aren't heights: without the hook they do nothing, rather than go in as other stamps do.
            Log::Write("terrain tiles: %s off: Flatten needs the peak stamp's calls, not hooked", set);
            continue;
        }
        std::vector<int> terrains;
        std::string listed;
        for (const std::string& name : ListNames(cfg.terrains))
        {
            const int t = find(terrainNames, name);
            if (t < 0)
                Log::Write("terrain tiles: %s: no terrain %s", set, name.c_str());
            else if (g_plotStampOf[t] >= 0 || std::find(terrains.begin(), terrains.end(), t) != terrains.end())
                Log::Write("terrain tiles: %s: %s is already in an earlier PlotStamps<n>Terrains", set, name.c_str());
            else
            {
                terrains.push_back(t);
                listed += (listed.empty() ? "" : ", ") + name;
            }
        }
        std::vector<int> onFeatures;
        for (const std::string& name : ListNames(cfg.onFeatures))
        {
            const int f = find(featureNames, name);
            if (f < 0)
                Log::Write("terrain tiles: %s: no feature %s", set, name.c_str());
            else if (g_plotStampOnFeature[f] >= 0 || std::find(onFeatures.begin(), onFeatures.end(), f) != onFeatures.end())
                Log::Write("terrain tiles: %s: %s is already in an earlier PlotStamps<n>OnFeatures", set, name.c_str());
            else
            {
                onFeatures.push_back(f);
                listed += (listed.empty() ? "" : ", ") + name;
            }
        }
        if (terrains.empty() && onFeatures.empty())
        {
            Log::Write("terrain tiles: %s off: no terrain or feature of %ls%ls to stamp", set, cfg.terrains.c_str(),
                       cfg.onFeatures.c_str());
            continue;
        }
        run.layer = onFeatures.empty() ? s + 1 : kPeakLayer;
        if (cfg.folder.empty())
        {
            Log::Write("terrain tiles: %s off: no %sFolder set", set, set);
            continue;
        }
        std::vector<uint8_t> bytes;
        std::wstring from;
        Jigsaw::PiecesXml xml;
        std::string why = "can't be read";
        if (!ReadPiecesFile(cfg.folder, "pieces.xml", bytes, from) ||
            !Jigsaw::ParsePiecesXml(std::string(bytes.begin(), bytes.end()), xml, why))
        {
            Log::Write("terrain tiles: %s off: %ls: %s", set, from.c_str(), why.c_str());
            continue;
        }
        int left = 0;
        uint16_t highest = 0;
        for (const Jigsaw::PieceFile& pf : xml.pieces)
        {
            std::vector<uint8_t> dds;
            std::wstring at = cfg.folder + L"\\" + std::wstring(pf.file.begin(), pf.file.end());
            std::vector<uint16_t> pixels;
            int w = 0, h = 0;
            Jigsaw::Piece p;
            std::string bad = "can't be read";
            if (pf.plots.size() != 1)
                bad = "not a one-plot layout";
            else if (ReadPiecesFile(cfg.folder, pf.file, dds, at) &&
                     Jigsaw::DecodeDds(dds.data(), dds.size(), w, h, pixels, bad) &&
                     Jigsaw::MakePiece(pf.file, pf.plots, w, h, std::move(pixels), p, bad))
            {
                for (uint16_t v : p.pixels)
                    highest = (std::max)(highest, v);
                run.pieces.push_back(std::move(p));
                continue;
            }
            if (left++ < 5)
                Log::Write("terrain tiles: %s: piece %ls left out: %s", set, at.c_str(), bad.c_str());
        }
        if (run.pieces.empty())
        {
            Log::Write("terrain tiles: %s off: no one-plot piece read from %ls", set, from.c_str());
            continue;
        }
        std::string features;
        for (const std::string& name : ListNames(cfg.features))
        {
            const int f = find(featureNames, name);
            if (f < 0)
                Log::Write("terrain tiles: %s: no feature %s (its place in PlotStamps<n>Features kept)", set,
                           name.c_str());
            const int k = static_cast<int>(run.features.size());
            if (k < static_cast<int>(run.pieces.size()))
                features += std::string(features.empty() ? "" : ", ") + name + " " + run.pieces[k].name;
            run.features.push_back(f);
        }
        if (cfg.textures && cfg.flatten)
            Log::Write("terrain tiles: %s: Textures doesn't apply with Flatten", set);
        if (cfg.textures && !cfg.flatten)
        {
            std::vector<int> layerTerrains = terrains;
            if (!onFeatures.empty())
            {
                const int peak = find(terrainNames, "TERRAIN_PEAK");
                layerTerrains.clear();
                if (peak >= 0)
                    layerTerrains.push_back(peak);
            }
            ReadPaintings(run, cfg, layerTerrains, !onFeatures.empty());
        }
        // Grid units a level: the INI's, else pieces.xml's world units over the grid's.
        const float level = cfg.hasHeight ? cfg.height : xml.heightScale / zScale;
        run.units = cfg.flatten ? PlotStamps::kFlattenUnits : level / Jigsaw::kValuePerLevel;
        char scale[96];
        if (cfg.hasHeight)
            snprintf(scale, sizeof(scale), "the INI's");
        else
            snprintf(scale, sizeof(scale), "pieces.xml's %.2f world units over fZScale %.2f%s", xml.heightScale, zScale,
                     zRead ? "" : " (assumed)");
        if (cfg.flatten)
            Log::Write("terrain tiles: %s: %s; %u one-plot pieces from %ls%s; by feature: %s; they level the ground",
                       set, listed.c_str(), static_cast<unsigned>(run.pieces.size()), from.c_str(),
                       left ? (", " + std::to_string(left) + " left out").c_str() : "",
                       features.empty() ? "none, by plot" : features.c_str());
        else
        Log::Write("terrain tiles: %s: %s; %u one-plot pieces from %ls%s; by feature: %s; %.3f grid units a level "
                   "(%s), the tallest %.1f grid units",
                   set, listed.c_str(), static_cast<unsigned>(run.pieces.size()), from.c_str(),
                   left ? (", " + std::to_string(left) + " left out").c_str() : "",
                   features.empty() ? "none, by plot" : features.c_str(), level, scale, highest * run.units);
        for (int t : terrains)
            g_plotStampOf[t] = static_cast<int>(g_plotStamps.size());
        for (int f : onFeatures)
            g_plotStampOnFeature[f] = static_cast<int>(g_plotStamps.size());
        g_plotStamps.push_back(std::move(run));
    }
    return !g_plotStamps.empty();
}

// The stamps on square (x, y)'s corner plots, as the plots are now, the Flatten sets' or the others'; how many.
int PlotStampsAt(int x, int y, PlotStamps::Stamp out[4], bool flatten = false)
{
    void* map = g_dll.Map();
    const int width = g_dll.width(map, nullptr), height = g_dll.height(map, nullptr);
    const bool wrapX = g_dll.wrapX(map, nullptr), wrapY = g_dll.wrapY(map, nullptr);
    int corners[4][2], count = 0;
    TileSets::Corners(x, y, height, corners);
    const bool painted = !g_paintings.dds.empty();
    if (painted)
        PaintingsMap(width, height, wrapX, wrapY);
    for (int k = 0; k < 4; ++k)
    {
        void* plot = g_dll.plot(map, nullptr, corners[k][0], corners[k][1]);
        if (!plot)
            continue;
        int px = corners[k][0], py = corners[k][1];  // on the map, for the pick: the same plot across a wrap
        if (wrapX && width > 0)
            px = ((px % width) + width) % width;
        if (wrapY && height > 0)
            py = ((py % height) + height) % height;
        const int feature = g_dll.featureType(plot, nullptr);
        const int s = PlotStamps::SetFor(feature, g_dll.terrainType(plot, nullptr), g_plotStampOnFeature, g_plotStampOf);
        uint8_t cell = 0, layer = 0;
        if (s >= 0)
        {
            const PlotStampRun& run = g_plotStamps[s];
            const int pick = PlotStamps::Pick(feature, run.features, static_cast<int>(run.pieces.size()), px, py);
            if (run.flatten == flatten)
                out[count++] = {&run.pieces[pick], corners[k][0], corners[k][1], run.units};
            if (!run.flatten)  // to the terrain shader a seat is no stamp
            {
                cell = static_cast<uint8_t>(run.cells.empty() ? 255 : run.cells[pick]);
                layer = static_cast<uint8_t>(run.layer);
            }
        }
        // The same pick as the height: what the terrain shader paints on this plot.
        if (painted && px >= 0 && py >= 0 && px < width && py < height)
        {
            uint8_t* at = &g_paintings.cells[(static_cast<size_t>(py) * width + px) * 2];
            if (at[0] != cell || at[1] != layer)
            {
                at[0] = cell;
                at[1] = layer;
                ++g_paintings.version;
            }
        }
    }
    return count;
}

// Before the engine's base stamp (land corner mask in, the mask to stamp with out): a square touching a set's plots
// is stamped with those corners as the set says, and its tile is added after (JigsawBaseAdd).
extern "C" int __stdcall JigsawBaseMask(int x, int y, int landMask)
{
    g_jigsawPending = false;
    if (!JigsawsReady())
        return landMask;
    if (!g_jigsaws[0].field.Built() || y < g_jigsawLastY || (y == g_jigsawLastY && x <= g_jigsawLastX))
        JigsawSync();
    g_jigsawLastX = x;
    g_jigsawLastY = y;
    int inSet = 0, toWater = 0, toLand = 0;
    for (const JigsawRun& j : g_jigsaws)
    {
        const int m = j.field.Mask(x, y);
        inSet |= m;
        toWater |= j.base == Jigsaw::kWater ? m : 0;
        toLand |= j.base == Jigsaw::kLand ? m : 0;
    }
    if (!inSet)
        return landMask;
    int corners[4][2], ownLand = 0;
    g_jigsaws[0].field.Corners(x, y, corners);
    for (int k = 0; k < 4; ++k)
    {
        const int i = HillField::PlotIndex(g_jigsawLand, corners[k][0], corners[k][1]);
        ownLand |= (i >= 0 && g_jigsawLand.hills[i]) ? 1 << k : 0;
    }
    bool add = false;
    const int mask = Jigsaw::BaseMask(landMask, inSet, ownLand, toWater, toLand, add);
    if (!add)
    {
        ++g_jigsawDisagree;
        return landMask;
    }
    ++g_jigsawSquares;
    g_jigsawPending = true;
    g_jigsawX = x;
    g_jigsawY = y;
    return mask;
}

// After it: the jigsaw sets' tiles for the square, combined, and the plot stamps on its corners on top, added into the
// height grid.
extern "C" void __stdcall JigsawBaseAdd(void* grid, int x, int y)
{
    const bool jigsaw = g_jigsawPending && x == g_jigsawX && y == g_jigsawY;
    g_jigsawPending = false;
    PlotStamps::Stamp stamps[4];
    const int stampCount = PlotStampsReady() ? PlotStampsAt(x, y, stamps) : 0;
    if (!jigsaw && !stampCount)
        return;
    Jigsaw::GridView g;
    if (!Jigsaw::ReadGrid(grid, g) || g.cellX > 256)
    {
        ++(jigsaw ? g_jigsawMisfit : g_stampMisfit);
        return;
    }
    const int n = g.cellX;
    g_jigsawTile.assign(static_cast<size_t>(n) * n, 0.0f);
    bool any = false;
    if (jigsaw)
    {
        g_jigsawSum.assign(static_cast<size_t>(n) * n, Jigsaw::Combine());
        for (const JigsawRun& j : g_jigsaws)
            if (j.field.Tile(x, y, n, g_jigsawTile.data()))
            {
                any = true;
                for (size_t i = 0; i < g_jigsawTile.size(); ++i)
                    g_jigsawSum[i].Add(g_jigsawTile[i]);
            }
        for (size_t i = 0; i < g_jigsawTile.size(); ++i)
            g_jigsawTile[i] = any ? g_jigsawSum[i].Value() : 0.0f;
    }
    if (stampCount)
    {
        g_stampTile.resize(static_cast<size_t>(n) * n);
        PlotStamps::Tile(stamps, stampCount, x, y, g_dll.height(g_dll.Map(), nullptr), n, g_stampTile.data());
        for (size_t i = 0; i < g_jigsawTile.size(); ++i)
            g_jigsawTile[i] += g_stampTile[i];
        g_stampClipped += PlotStamps::Clipped(g.cells, g.rowBytes, g.rows, x * n, y * n, n, g_jigsawTile.data(),
                                              g_stampTile.data());
    }
    if (!any && !stampCount)
        return;
    if (Jigsaw::AddTile(g.cells, g.rowBytes, g.rows, x * n, y * n, n, g_jigsawTile.data()))
    {
        g_jigsawAdded += any ? 1 : 0;
        g_stampSquares += stampCount ? 1 : 0;
    }
    else
        ++(any ? g_jigsawMisfit : g_stampMisfit);
}

// After the peak stamp, the last of a square's stamps: PlotStamps<n>Flatten, each corner's seat in turn.
extern "C" void __stdcall PlotFlattenSquare(void* grid, int x, int y)
{
    PlotStamps::Stamp stamps[4];
    const int count = PlotStampsReady() ? PlotStampsAt(x, y, stamps, true) : 0;
    if (!count)
        return;
    Jigsaw::GridView g;
    if (!Jigsaw::ReadGrid(grid, g) || g.cellX > 256)
    {
        ++g_stampMisfit;
        return;
    }
    const int n = g.cellX, gridHeight = g_dll.height(g_dll.Map(), nullptr);
    g_stampTile.resize(static_cast<size_t>(n) * n);
    bool any = false;
    for (int k = 0; k < count; ++k)
    {
        PlotStamps::Tile(&stamps[k], 1, x, y, gridHeight, n, g_stampTile.data());
        if (!g_flattenEdgeLogged && PlotStamps::OnEdge(g_stampTile.data(), n))
        {
            g_flattenEdgeLogged = true;
            Log::Write("terrain tiles: warning: Flatten piece %s has weight on the edge of square %d, %d, so a step there",
                       stamps[k].piece->name.c_str(), x, y);
        }
        if (PlotStamps::Flatten(g.cells, g.rowBytes, g.rows, x * n, y * n, n, g_stampTile.data()))
            any = true;
    }
    g_flattenSquares += any ? 1 : 0;
}

extern "C" uint32_t g_flattenNextPeak = 0;  // what the peak stamp's calls called (PlotStamps::WrapCalls)

// The peak stamp's calls come here: (landscape, x) on the stack, its RET 8 pops them, the mask in EAX and y in EBX.
extern "C" __declspec(naked) void PlotFlattenThunk()
{
    __asm {
        push dword ptr [esp + 8]    // x
        push dword ptr [esp + 8]    // landscape
        call dword ptr [g_flattenNextPeak]
        push ebx                    // y
        push dword ptr [esp + 0xc]  // x
        push dword ptr [esp + 0xc]  // landscape
        call PlotFlattenSquare
        ret 8
    }
}

extern "C" uint32_t g_jigsawBaseResume = 0;  // FUN_00478a90 + 6, after the two instructions the hook took
extern "C" __declspec(naked) void JigsawBaseOriginal()
{
    __asm {
        sub esp, 0x10
        cmp edi, 1
        jmp dword ptr [g_jigsawBaseResume]
    }
}

extern "C" __declspec(naked) void JigsawBaseThunk()
{
    __asm {
        push edi                   // the caller's land mask, given back after
        push edi                   // land mask
        push ebx                   // y
        push dword ptr [esp + 20]  // x
        call JigsawBaseMask
        mov edi, eax
        push dword ptr [esp + 12]  // x
        push dword ptr [esp + 12]  // the height grid
        call JigsawBaseOriginal    // the engine's stamp, with the mask (RET 8)
        push ebx                   // y
        push dword ptr [esp + 16]  // x
        push dword ptr [esp + 16]  // the height grid
        call JigsawBaseAdd
        pop edi
        ret 8
    }
}

// The engine's picker, set-aware. Same seed and the exe's own srand/rand, so with one set it picks what BtS picks.
extern "C" int __stdcall TileSetsPicker(int x, int y, int lo, int hi, uint32_t returnAddress, uint32_t esi)
{
    int wx = x, wy = y;
    Wrap(wx, wy);
    using Srand = void(__cdecl*)(unsigned);
    using Rand = int(__cdecl*)();
    (*reinterpret_cast<Srand*>(g_exe->srand))(TileSets::Seed(wx, wy));
    const int r = (*reinterpret_cast<Rand*>(g_exe->rand))();
    const int count = hi - lo;
    if (count <= 0)
        return lo;
    if (g_sets <= 1 || returnAddress == g_base + g_exe->blendCaller)
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
    const uint32_t list = family ? TileSets::PeakList(*g_exe) : TileSets::HillList(*g_exe);
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
    if (!g_config.tileSetsPatch && !g_config.hillField && !AnyJigsaw() && !AnyPlotStamps())
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
        Log::Write("terrain tile groups: not applied: %ls isn't Civ4BeyondSword.exe", name.c_str());
        return;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (reinterpret_cast<uint32_t>(base) != 0x400000)
    {
        Log::Write("terrain tile groups: not applied: the exe is loaded at %p, not 0x400000", base);
        return;
    }
    const Exe::Layout* exeLayout = Exe::Identify(base, nt->OptionalHeader.SizeOfImage);
    if (!exeLayout)
    {
        Log::Write("terrain tile groups: not applied: not a BtS 3.1.9.0 build it knows (Steam's or GOG's; exe timestamp "
                   "0x%08lx)", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return;
    }
    g_exe = exeLayout;
    g_base = reinterpret_cast<uint32_t>(base);
    g_tileSetsCoastClassFn = g_base + g_exe->coastClassFn;
    const auto sites = TileSets::Sites(g_base, reinterpret_cast<uint32_t>(&TileSetsPickerThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsHillThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsPeakThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsAtlasThunk),
                                       reinterpret_cast<uint32_t>(&TileSetsWaveThunk), *g_exe);
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
        Log::Write("terrain tile groups: not applied: %s (%s build, exe timestamp 0x%08lx)", why.c_str(), g_exe->name,
                   static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
    else
        Log::Write("terrain tile groups: %d of %u code sites written (%s build: picker, hill and peak lookups, coast atlas "
                   "and wave models); %d tile group%s; coast art per class: atlas cells %d,%d,%d,%d, wave models "
                   "%d,%d,%d,%d",
                   written, static_cast<unsigned>(sites.size()), g_exe->name, g_sets, g_sets == 1 ? "" : "s",
                   g_config.coastAtlasCells[0], g_config.coastAtlasCells[1], g_config.coastAtlasCells[2],
                   g_config.coastAtlasCells[3], g_config.coastWaveModels[0], g_config.coastWaveModels[1],
                   g_config.coastWaveModels[2], g_config.coastWaveModels[3]);
    if (!why.empty() || (!AnyJigsaw() && !AnyPlotStamps()))
        return;
    g_jigsawBaseResume = g_base + g_exe->baseStamp + 6;
    int jigsawWrites = 0;
    const std::string jigsaw = TerrainPatch::ApplySites(base, nt->OptionalHeader.SizeOfImage,
                                                        Jigsaw::Sites(g_base, reinterpret_cast<uint32_t>(&JigsawBaseThunk),
                                                                      *g_exe),
                                                        [&](uint32_t rva, const uint8_t* bytes, size_t count) {
        void* at = const_cast<uint8_t*>(base) + rva;
        DWORD old = 0;
        if (VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(at, bytes, count);
            VirtualProtect(at, count, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, count);
            ++jigsawWrites;
        }
    });
    if (jigsaw.empty())
        Log::Write("terrain tiles: jigsaw pieces, plot stamps: the base tile stamp hooked (%d site written)",
                   jigsawWrites);
    else
        Log::Write("terrain tiles: jigsaw pieces, plot stamps: not applied: %s", jigsaw.c_str());
}

// [terrain tiles] StandardBlendOnLand(1..8) (terrain_sets.h): the engine's blend pick, then on a square with no water
// corner a land entry for terrains that have them.
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
unsigned g_blendedSquares = 0;  // MixedCoastBlend: squares whose paired land layers blend

// [terrain tiles] MixedCoastBlend: what the terrain shader needs to weight a blended square's paired land layers
// (terrain_shadow.hlsl, MixWeight). Per plot: r = terrain + 1, g = 255 where water, 128 on a peak plot (when the peak
// layer is paired), b = the terrain's rank among the paired land terrains by layer order (1, 2...; 0: none), a = 255
// where the square with this plot at its south-west corner (its lowest x and y) is blended. A peak plot keeps its own
// terrain and rank: the engine builds that terrain's layer there, so the layer must find its corner, or it's drawn whole.
// Filled by the blend pick hook as the engine builds the layers.
struct MixData
{
    bool tried = false, on = false;
    int peak = -1;  // TERRAIN_PEAK, if it's a paired layer
    int width = 0, height = 0;
    bool wrapX = false, wrapY = false;
    float toPlot[3] = {};
    std::vector<uint8_t> rgba;
    std::vector<int> rank;  // by TerrainTypes
    std::vector<MixedCoastSheet> sheets;
    unsigned version = 0;
    int squareX = -0x7fffffff, squareY = -0x7fffffff;
} g_mix;

// Once, and again for a map of another size; false (logged once) if it can't run.
bool MixReady()
{
    if (!g_config.mixedCoastBlend || !(g_config.lightingEnabled || g_config.shadowsEnabled))
        return false;
    void* map = g_dll.Map();
    const int w = g_dll.width(map, nullptr), h = g_dll.height(map, nullptr);
    if (g_mix.tried && (!g_mix.on || (w == g_mix.width && h == g_mix.height)))
        return g_mix.on;
    const bool first = !g_mix.tried;
    g_mix.tried = true;
    g_mix.on = false;
    if (!g_dll.artDefineTag || !g_dll.artFileMgr || !g_dll.terrainArt || !g_dll.layerOrder || !g_dll.baseTexture ||
        !g_dll.defineFloat || w <= 0 || h <= 0 || w > 1024 || h > 1024)
    {
        Log::Write("terrain land blends: MixedCoastBlend off: the game DLL lacks the terrain art or PLOT_SIZE exports");
        return false;
    }
    // Plot centres, as CvMap::plotXToPointX and plotYToPointY give them once the landscape is up (it isn't while the
    // layers are built): evenly spaced round the middle of the map.
    void* gc = g_dll.getInstance();
    const float dx = g_dll.defineFloat(gc, nullptr, "PLOT_SIZE");
    if (dx < 1)
    {
        Log::Write("terrain land blends: MixedCoastBlend off: PLOT_SIZE is %.1f", dx);
        return false;
    }
    const float x0 = (0.5f - w * 0.5f) * dx, y0 = (0.5f - h * 0.5f) * dx;
    const int n = static_cast<int>(g_landBlendFor.size());
    struct Paired
    {
        int terrain, order;
        std::string name, sheet, grid;
    };
    std::vector<Paired> paired;
    for (int t = 0; t < n; ++t)
    {
        if (g_landBlendFor[t] < 0)
            continue;
        void* info = g_dll.terrainInfo(gc, nullptr, t);
        const char* tag = g_dll.artDefineTag(info, nullptr);
        void* art = tag ? g_dll.terrainArt(g_dll.artFileMgr(), nullptr, tag) : nullptr;
        const char* sheet = art ? g_dll.baseTexture(art, nullptr) : nullptr;
        const char* grid = art && g_dll.gridTexture ? g_dll.gridTexture(art, nullptr) : nullptr;
        if (art && sheet && *sheet)
            paired.push_back({t, g_dll.layerOrder(art, nullptr), g_dll.type(info, nullptr), sheet, grid ? grid : ""});
    }
    std::stable_sort(paired.begin(), paired.end(), [](const Paired& a, const Paired& b) { return a.order < b.order; });
    g_mix.rank.assign(n, 0);
    g_mix.sheets.clear();
    g_mix.peak = -1;
    std::string listed;
    for (size_t k = 0; k < paired.size(); ++k)
    {
        g_mix.rank[paired[k].terrain] = static_cast<int>(k) + 1;
        const bool peak = g_dll.isPeak && _stricmp(paired[k].name.c_str(), "TERRAIN_PEAK") == 0;
        if (peak)
            g_mix.peak = paired[k].terrain;
        g_mix.sheets.push_back({std::wstring(paired[k].sheet.begin(), paired[k].sheet.end()), paired[k].terrain + 1,
                                static_cast<int>(k) + 1, peak});
        if (!paired[k].grid.empty())  // what's bound with the grid on
            g_mix.sheets.push_back({std::wstring(paired[k].grid.begin(), paired[k].grid.end()), paired[k].terrain + 1,
                                    static_cast<int>(k) + 1, peak});
        listed += (listed.empty() ? "" : ", ") + paired[k].name + " (layer " + std::to_string(paired[k].order) + ", " +
                  paired[k].sheet + ")";
    }
    g_mix.width = w;
    g_mix.height = h;
    g_mix.wrapX = g_dll.wrapX(map, nullptr);
    g_mix.wrapY = g_dll.wrapY(map, nullptr);
    g_mix.toPlot[0] = 1.0f / dx;
    g_mix.toPlot[1] = -x0 / dx;
    g_mix.toPlot[2] = -y0 / dx;
    g_mix.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    ++g_mix.version;
    g_mix.on = paired.size() >= 2;
    if (first || g_mix.on)
        Log::Write("terrain land blends: MixedCoastBlend %s: %s; a %dx%d map, %.1f world units a plot%s",
                   g_mix.on ? "on" : "off (fewer than two land terrains with paired lists)",
                   listed.empty() ? "none" : listed.c_str(), w, h, dx,
                   g_mix.peak >= 0 ? "; peak plots blend as TERRAIN_PEAK" : "");
    return g_mix.on;
}

// A plot's place in g_mix.rgba (wrapped onto the map), or -1 off it.
int MixIndex(int px, int py)
{
    if (g_mix.wrapX)
        px = ((px % g_mix.width) + g_mix.width) % g_mix.width;
    if (g_mix.wrapY)
        py = ((py % g_mix.height) + g_mix.height) % g_mix.height;
    if (px < 0 || py < 0 || px >= g_mix.width || py >= g_mix.height)
        return -1;
    return (py * g_mix.width + px) * 4;
}

void MixSet(int at, uint8_t value)
{
    if (at >= 0 && g_mix.rgba[at] != value)
    {
        g_mix.rgba[at] = value;
        ++g_mix.version;
    }
}
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
    const bool mix = MixReady();
    if (mix)
    {
        // The corners as the shader reads them, and the square's mark cleared as the engine starts on it.
        for (int k = 0; k < 4; ++k)
        {
            const int i = MixIndex(at[k][0], at[k][1]);
            if (i < 0)
                continue;
            void* plot = g_mix.peak >= 0 ? g_dll.plot(map, nullptr, at[k][0], at[k][1]) : nullptr;
            const bool peak = plot && g_dll.isPeak(plot, nullptr);
            const int t = corners.terrain[k];
            MixSet(i, static_cast<uint8_t>(t >= 0 && t < 254 ? t + 1 : 0));
            MixSet(i + 1, corners.water[k] ? 255 : peak ? 128 : 0);
            MixSet(i + 2, static_cast<uint8_t>(t >= 0 && t < static_cast<int>(g_mix.rank.size()) ? g_mix.rank[t] : 0));
        }
        if (x != g_mix.squareX || y != g_mix.squareY)
        {
            g_mix.squareX = x;
            g_mix.squareY = y;
            const int i = MixIndex(x - 1, top - 1);
            MixSet(i < 0 ? -1 : i + 3, 0);
        }
    }
    const TileSets::BlendAction action = g_blendSquare.Next(x, y, corners, terrain, mask, own >= 0, fullMask, mix);
    if (mix && g_blendSquare.blended)
    {
        const int i = MixIndex(x - 1, top - 1);
        if (i >= 0 && g_mix.rgba[i + 3] != 255)
            ++g_blendedSquares;
        MixSet(i < 0 ? -1 : i + 3, 255);
    }
    switch (action)
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

void ApplyPlotFlatten()
{
    bool any = false;
    for (const auto& p : g_config.plotStamps)
        any = any || (p.flatten && !(p.terrains.empty() && p.onFeatures.empty()));
    if (!any)
        return;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = exe;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    for (auto& c : name)
        c = static_cast<wchar_t>(towlower(c));
    if (name != L"civ4beyondsword.exe")
    {
        Log::Write("terrain tiles: Flatten: not applied: %ls isn't Civ4BeyondSword.exe", name.c_str());
        return;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const Exe::Layout* exeLayout = reinterpret_cast<uint32_t>(base) == 0x400000
                                       ? Exe::Identify(base, nt->OptionalHeader.SizeOfImage)
                                       : nullptr;
    if (!exeLayout)
    {
        Log::Write("terrain tiles: Flatten: not applied: not a BtS 3.1.9.0 build it knows, at 0x400000 (exe timestamp "
                   "0x%08lx)", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return;
    }
    std::vector<TerrainPatch::Site> sites;
    std::string why;
    const uint32_t at = reinterpret_cast<uint32_t>(base);
    if (PlotStamps::WrapCalls(base, nt->OptionalHeader.SizeOfImage, at, *exeLayout,
                              reinterpret_cast<uint32_t>(&PlotFlattenThunk), sites, g_flattenNextPeak, why))
    {
        size_t written = 0;
        why = TerrainPatch::ApplySites(base, nt->OptionalHeader.SizeOfImage, sites,
                                       [&](uint32_t rva, const uint8_t* bytes, size_t count) {
            void* to = const_cast<uint8_t*>(base) + rva;
            DWORD old = 0;
            if (VirtualProtect(to, count, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(to, bytes, count);
                VirtualProtect(to, count, old, &old);
                FlushInstructionCache(GetCurrentProcess(), to, count);
                ++written;
            }
        });
        g_flattenHooked = why.empty() && written == sites.size();
        if (g_flattenHooked)
            Log::Write("terrain tiles: Flatten: the peak stamp's %u calls wrapped (%s build)%s",
                       static_cast<unsigned>(sites.size()), exeLayout->name,
                       g_flattenNextPeak == at + exeLayout->peakStamp ? "" : ", round another patch's wrapper");
    }
    if (!g_flattenHooked)
        Log::Write("terrain tiles: Flatten: not applied: %s (%s build)", why.empty() ? "a write failed" : why.c_str(),
                   exeLayout->name);
}

void ApplyLandBlends()
{
    // StandardBlendOnLand1..8 (the full form) first: they win over the short form's standard layout for the same
    // terrain.
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
    const Exe::Layout* exeLayout = Exe::Identify(base, nt->OptionalHeader.SizeOfImage);
    if (!exeLayout)
    {
        Log::Write("terrain land blends: not applied: not a BtS 3.1.9.0 build it knows (Steam's or GOG's; exe timestamp "
                   "0x%08lx)", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return;
    }
    g_exe = exeLayout;
    g_base = reinterpret_cast<uint32_t>(base);
    g_blendPickFn = g_base + g_exe->blendPick;
    const auto sites = TileSets::LandBlendSites(g_base, reinterpret_cast<uint32_t>(&LandBlendThunk), *g_exe);
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
        Log::Write("terrain land blends: not applied: %s (%s build, exe timestamp 0x%08lx)", why.c_str(), g_exe->name,
                   static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
    else
        Log::Write("terrain land blends: %d of %u code sites written (%s build); %u terrain%s with land entries", written,
                   static_cast<unsigned>(sites.size()), g_exe->name, static_cast<unsigned>(g_landBlends.size()),
                   g_landBlends.size() == 1 ? "" : "s");
}

namespace
{
void(__stdcall* g_layerBuilder)(void*) = nullptr;
int g_riverTerrain = -2, g_riverOrder = 0;  // -2: not looked up yet; -1: it can't run
unsigned g_riverLogged = 0xffffffffu;
RiverLayer::Shapes g_shapes;
bool g_shapesRead = false;
RiverLayer::Cuts g_cuts;
float g_cutZScale = 1.0f;
unsigned g_capSquares = 0, g_capLogged = 0;

// The shape tiles, once.
void ReadShapes()
{
    if (g_shapesRead)
        return;
    g_shapesRead = true;
    // RiverLayerDepths wins: its tiles lower the ground rather than cap it
    const std::wstring& folder = g_config.riverLayerDepths.empty() ? g_config.riverLayerCaps : g_config.riverLayerDepths;
    if (folder.empty())
        return;
    for (const RiverLayer::ShapeId& id : RiverLayer::kOrder)
    {
        g_shapes.first[id.kind][id.arms] = static_cast<int>(g_shapes.tiles.size());
        for (int n = 1;; ++n)
        {
            const std::string name = RiverLayer::CapFile(id.kind, id.arms, n);
            std::vector<uint8_t> file, cap;
            std::wstring from;
            if (!ReadPiecesFile(folder, name, file, from))
                break;
            if (!RiverLayer::ParseCap(file, cap))
            {
                Log::Write("terrain river layer: %s isn't a 64 x 64 8-bit grey TGA, left out", name.c_str());
                continue;
            }
            g_shapes.tiles.push_back(std::move(cap));
            ++g_shapes.count[id.kind][id.arms];
        }
    }
    Log::Write("terrain river layer: %u shape tiles from %ls", static_cast<unsigned>(g_shapes.tiles.size()), folder.c_str());
}

// The tile for square (x, y), or -1: its arms from the plots round its middle, and which of them are water or sinks.
int ShapeAt(void* map, int x, int y, int h, int w, bool wrapX, int* armsOut)
{
    int c[4][2];
    TileSets::Corners(x, y, h, c);  // TL, TR, BR, BL
    void* plots[4];
    for (int k = 0; k < 4; ++k)
        plots[k] = g_dll.plot(map, nullptr, c[k][0], c[k][1]);
    void* tl = plots[0];
    void* tr = plots[1];
    void* bl = plots[3];
    const int arms = RiverLayer::Arms(tl && g_dll.isWOfRiver(tl, nullptr), bl && g_dll.isWOfRiver(bl, nullptr),
                                      tl && g_dll.isNOfRiver(tl, nullptr), tr && g_dll.isNOfRiver(tr, nullptr));
    if (armsOut)
        *armsOut = arms;
    if (arms == 0 || g_shapes.tiles.empty())
        return -1;
    int open = 0;
    for (int k = 0; k < 4; ++k)
        if (plots[k] && ((g_dll.isWater && g_dll.isWater(plots[k], nullptr)) || (g_dll.isHills && g_dll.isHills(plots[k], nullptr))))
            open |= 1 << k;
    return RiverLayer::ShapeTile(g_shapes, arms, open, wrapX && w > 0 ? x % w : x, y);
}

// The terrain's number and layer order, the first time the records are built (the DLL's infos are loaded by then).
void FindRiverTerrain()
{
    g_riverTerrain = -1;
    if (!g_dll.Resolve() || !g_dll.isNOfRiver || !g_dll.isWOfRiver || !g_dll.artDefineTag || !g_dll.artFileMgr ||
        !g_dll.terrainArt || !g_dll.layerOrder)
    {
        Log::Write("terrain river layer: off: the game DLL lacks the river or terrain art exports");
        return;
    }
    const std::wstring& w = g_config.riverLayerTerrain;
    const std::string wanted(w.begin(), w.end());
    void* gc = g_dll.getInstance();
    const int count = g_dll.numTerrains(gc, nullptr);
    for (int t = 0; t < count; ++t)
    {
        void* info = g_dll.terrainInfo(gc, nullptr, t);
        const char* name = g_dll.type(info, nullptr);
        if (!name || _stricmp(name, wanted.c_str()) != 0)
            continue;
        const char* tag = g_dll.artDefineTag(info, nullptr);
        void* art = tag ? g_dll.terrainArt(g_dll.artFileMgr(), nullptr, tag) : nullptr;
        if (art)
        {
            g_riverTerrain = t;
            g_riverOrder = g_dll.layerOrder(art, nullptr);
        }
        break;
    }
    if (g_riverTerrain < 0)
        Log::Write("terrain river layer: off: no terrain %s with art", wanted.c_str());
}

// Runs in place of the layer record builder (river_layer.h).
void __stdcall RiverLayerBuilder(void* landscape)
{
    g_layerBuilder(landscape);
    if (g_riverTerrain == -2)
        FindRiverTerrain();
    if (g_riverTerrain < 0)
        return;
    ReadShapes();
    void* map = g_dll.Map();
    const int w = g_dll.width(map, nullptr), h = g_dll.height(map, nullptr);
    const bool wrapX = g_dll.wrapX(map, nullptr);
    uint8_t* records = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(landscape) + RiverLayer::kRecordsAt);
    unsigned added = 0, full = 0;
    for (int y = 0; y <= h; ++y)
        for (int x = 0; x <= w; ++x)
        {
            int arms = 0;
            const int shape = ShapeAt(map, x, y, h, w, wrapX, &arms);
            if (arms == 0)
                continue;
            const int tile = shape >= 0 ? g_config.riverLayerFirstTile + shape : RiverLayer::Tile(arms, g_config.riverLayerFirstTile);
            uint8_t* record = records + (static_cast<size_t>(y) * (w + 1) + x) * RiverLayer::kRecordBytes;
            if (RiverLayer::Add(record, g_riverTerrain, tile, g_riverOrder))
                ++added;
            else
                ++full;
        }
    if (added + full * 65536 != g_riverLogged)
    {
        g_riverLogged = added + full * 65536;
        Log::Write("terrain river layer: on %u squares (%u had no free layer), terrain %d, order %d", added, full,
                   g_riverTerrain, g_riverOrder);
    }
}
}  // namespace

extern "C" uint32_t g_peakStampFn = 0;

// After the peak stamp, the last of a square's stamps (river_layer.h).
extern "C" void __stdcall RiverCapSquare(uint8_t* landscape, int x, int y)
{
    if (!g_dll.Resolve() || !g_dll.isNOfRiver || !g_dll.isWOfRiver)
        return;
    ReadShapes();
    if (g_shapes.tiles.empty())
        return;
    uint8_t* image = *reinterpret_cast<uint8_t**>(landscape + 4);
    if (!image)
        return;
    const int cellW = *reinterpret_cast<int*>(landscape + 0x30) - 1, cellH = *reinterpret_cast<int*>(landscape + 0x34) - 1;
    const int stride = *reinterpret_cast<int*>(image + 0x50);
    uint8_t* pixels = *reinterpret_cast<uint8_t**>(image + 0x58);
    if (!pixels || cellW != RiverLayer::kCapSize || cellH != RiverLayer::kCapSize)
        return;
    void* map = g_dll.Map();
    const int w = g_dll.width(map, nullptr), h = g_dll.height(map, nullptr);
    if (!g_cuts.Mapped())
    {
        // Plot centres as CvMap::plotXToPointX/plotYToPointY give them, evenly spaced round the middle of the map
        void* gc = g_dll.getInstance();
        const float plot = g_dll.defineFloat ? g_dll.defineFloat(gc, nullptr, "PLOT_SIZE") : 0.0f;
        const float step = plot >= 1 ? plot : 180.0f;
        if (g_dll.activeLandscape && g_dll.landscapeInfo && g_dll.zScale)
            if (void* info = g_dll.landscapeInfo(gc, nullptr, g_dll.activeLandscape(gc, nullptr)))
            {
                const float z = g_dll.zScale(info, nullptr);
                if (z > 0)
                    g_cutZScale = z;
            }
        g_cuts.Map(w, h, g_dll.wrapX(map, nullptr), (0.5f - w * 0.5f) * step, (0.5f - h * 0.5f) * step, step);
    }
    const int shape = ShapeAt(map, x, y, h, w, g_dll.wrapX(map, nullptr), nullptr);
    if (shape < 0)
    {
        g_cuts.Clear(x, y);  // restamped without a river: nothing cut here now
        return;
    }
    uint8_t* square = pixels + static_cast<size_t>(cellH) * stride * y + static_cast<size_t>(cellW) * x;
    uint8_t before[RiverLayer::kCapSize * RiverLayer::kCapSize];
    for (int r = 0; r < RiverLayer::kCapSize; ++r)
        memcpy(before + r * RiverLayer::kCapSize, square + static_cast<size_t>(r) * stride, RiverLayer::kCapSize);
    if (!g_config.riverLayerDepths.empty())
        RiverLayer::ApplyDepth(square, stride, g_shapes.tiles[shape].data());
    else
        RiverLayer::ApplyCap(square, stride,
                         g_shapes.tiles[shape].data());
    g_cuts.Set(x, y, before, square, stride);
    ++g_capSquares;
}

float RiverCutAt(float wx, float wy)
{
    return g_cuts.Bridge(wx, wy) * g_cutZScale;  // shallow cuts bridged, mesa gorges followed down
}

unsigned RiverCutVersion()
{
    return g_cuts.Version();
}

float RiverCutRawAt(float wx, float wy)
{
    return g_cuts.At(wx, wy) * g_cutZScale;
}

std::string RiverCutDescribe()
{
    char z[48];
    snprintf(z, sizeof(z), "\"zscale\":%.3f,", g_cutZScale);
    return z + g_cuts.Describe();
}

// The peak stamp's calls come here: (landscape, x) on the stack, its RET 8 pops them, the mask in EAX and y in EBX.
extern "C" __declspec(naked) void RiverPeakThunk()
{
    __asm {
        push dword ptr [esp + 8]    // x
        push dword ptr [esp + 8]    // landscape
        call dword ptr [g_peakStampFn]
        push ebx                    // y
        push dword ptr [esp + 0xc]  // x
        push dword ptr [esp + 0xc]  // landscape
        call RiverCapSquare
        ret 8
    }
}

void ApplyRiverLayer()
{
    if (g_config.riverLayerTerrain.empty())
        return;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = exe;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    for (auto& c : name)
        c = static_cast<wchar_t>(towlower(c));
    if (name != L"civ4beyondsword.exe")
    {
        Log::Write("terrain river layer: not applied: %ls isn't Civ4BeyondSword.exe", name.c_str());
        return;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (reinterpret_cast<uint32_t>(base) != 0x400000)
    {
        Log::Write("terrain river layer: not applied: the exe is loaded at %p, not 0x400000", base);
        return;
    }
    const Exe::Layout* exeLayout = Exe::Identify(base, nt->OptionalHeader.SizeOfImage);
    if (!exeLayout)
    {
        Log::Write("terrain river layer: not applied: not a BtS 3.1.9.0 build it knows (exe timestamp 0x%08lx)",
                   static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return;
    }
    const uint32_t at = reinterpret_cast<uint32_t>(base);
    g_layerBuilder = reinterpret_cast<void(__stdcall*)(void*)>(at + exeLayout->layerBuilder);
    g_peakStampFn = at + exeLayout->peakStamp;
    auto sites = RiverLayer::Sites(at, reinterpret_cast<uint32_t>(&RiverLayerBuilder), *exeLayout);
    if (!g_config.riverLayerCaps.empty() || !g_config.riverLayerDepths.empty())
        for (const auto& site : RiverLayer::CapSites(at, reinterpret_cast<uint32_t>(&RiverPeakThunk), *exeLayout))
            sites.push_back(site);
    int written = 0;
    const std::string why = TerrainPatch::ApplySites(base, nt->OptionalHeader.SizeOfImage, sites,
                                                     [&](uint32_t rva, const uint8_t* bytes, size_t count) {
        void* to = const_cast<uint8_t*>(base) + rva;
        DWORD old = 0;
        if (VirtualProtect(to, count, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(to, bytes, count);
            VirtualProtect(to, count, old, &old);
            FlushInstructionCache(GetCurrentProcess(), to, count);
            ++written;
        }
    });
    if (!why.empty())
        Log::Write("terrain river layer: not applied: %s (%s build)", why.c_str(), exeLayout->name);
    else
        Log::Write("terrain river layer: %d of %u code sites written (%s build); %ls, tiles from %d", written,
                   static_cast<unsigned>(sites.size()), exeLayout->name, g_config.riverLayerTerrain.c_str(),
                   g_config.riverLayerFirstTile);
}

std::string LandBlendsSummary()
{
    static unsigned logged = 0;
    unsigned total = 0;
    for (unsigned n : g_landPicks)
        total += n;
    total += g_mixedSquares + g_overlaySquares + g_blendedSquares;
    if (total == logged)
        return "";
    logged = total;
    std::string s = "terrain land blends: land entries given:";
    for (size_t k = 0; k < g_landBlends.size(); ++k)
        s += " " + g_landBlends[k].terrain + "=" + std::to_string(g_landPicks[k]);
    s += "; land layers of coast squares of two or more land terrains painted for the whole land: " +
         std::to_string(g_mixedSquares) + " (squares blending their paired land terrains, MixedCoastBlend: " +
         std::to_string(g_blendedSquares) + "); coast squares where a hill or peak layer covering part of the land "
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
    total += g_jigsawSquares + g_jigsawAdded + g_jigsawMisfit + g_jigsawDisagree + g_jigsawFits + g_jigsawUpdates;
    total += g_stampSquares + g_stampClipped + g_stampMisfit + g_flattenSquares;
    if (total == g_logged)
        return "";
    g_logged = total;
    // Numbered from 1, as the INI numbers the tile groups (TileGroup1, TileGroup2...) and the files (hill1_1-15 is
    // variant 1).
    std::string s = "terrain tile groups: tiles picked by tile group:";
    for (int k = 0; k < g_sets; ++k)
        s += " TileGroup" + std::to_string(k + 1) + "=" + std::to_string(g_picks[k]);
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
        s += (k ? "/TileGroup" : "TileGroup") + std::to_string(k + 1);
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
    if (g_jigsawSquares || g_jigsawDisagree || g_jigsawMisfit)
        s += "; jigsaw pieces: " + std::to_string(g_jigsawSquares) + " squares, " + std::to_string(g_jigsawAdded) +
             " tiles added, " + std::to_string(g_jigsawDisagree) + " left to the engine (a set's corner not land or "
             "water as read), " + std::to_string(g_jigsawMisfit) + " not fitting the grid, " +
             std::to_string(g_jigsawFits) + " placings, " + std::to_string(g_jigsawUpdates) + " local updates";
    if (g_stampSquares || g_stampMisfit)
        s += "; plot stamps: " + std::to_string(g_stampSquares) + " squares, " + std::to_string(g_stampClipped) +
             " samples clipped at 255 (no headroom), " + std::to_string(g_stampMisfit) + " not fitting the grid";
    if (g_flattenSquares)
        s += "; flattened: " + std::to_string(g_flattenSquares) + " squares";
    if (!g_setFlats.empty() && !g_setFlatsLogged)
    {
        g_setFlatsLogged = g_setFlats.size() >= 12;
        s += "; flat squares not in TileGroup1 (corner plots x,y to x+1,y+1):";
        for (const Square& q : g_setFlats)
            s += " (" + std::to_string(q.x0) + "," + std::to_string(q.y0) + " TileGroup" + std::to_string(q.set + 1) +
                 ")";
    }
    return s;
}

bool PlotStampsOn()
{
    return g_plotStampsRead && !g_plotStamps.empty();
}

std::wstring PlotStampsHiddenModels()
{
    std::wstring all;
    for (const auto& p : g_config.plotStamps)
        if ((!p.terrains.empty() || !p.onFeatures.empty()) && !p.hideModels.empty())
            all += (all.empty() ? L"" : L";") + p.hideModels;
    return all;
}

bool MixedCoastPlots(MixedCoastMap& out)
{
    if (!g_mix.on)
        return false;
    out.rgba = g_mix.rgba.data();
    out.width = g_mix.width;
    out.height = g_mix.height;
    out.wrapX = g_mix.wrapX;
    out.wrapY = g_mix.wrapY;
    out.version = g_mix.version;
    memcpy(out.toPlot, g_mix.toPlot, sizeof(out.toPlot));
    return true;
}

std::vector<MixedCoastSheet> MixedCoastSheets()
{
    return g_mix.on ? g_mix.sheets : std::vector<MixedCoastSheet>();
}

bool PlotStampsCells(PlotStampsMap& out)
{
    // [debug] PlotStampsTest=<folder>;x;y[;layer]: a 16 x 16 map with a stamp at plot x, y, painted with the last of
    // <folder>\a.dds, b.dds... (each its own cell of the atlas); layer: the plot's, if not the drawn layer's.
    if (!g_config.plotStampsTest.empty() && !g_paintings.testTried)
    {
        g_paintings.testTried = true;
        std::vector<std::wstring> parts;
        for (size_t from = 0; from <= g_config.plotStampsTest.size();)
        {
            const size_t end = (std::min)(g_config.plotStampsTest.find(L';', from), g_config.plotStampsTest.size());
            parts.push_back(g_config.plotStampsTest.substr(from, end - from));
            from = end + 1;
        }
        std::string why = "can't be read";
        for (wchar_t name = L'a'; parts.size() >= 3 && name <= L'h'; ++name)
        {
            std::vector<uint8_t> dds;
            PlotStamps::ColourFile colour;
            if (!ReadFileBytes(parts[0] + L"\\" + name + L".dds", dds) ||
                !PlotStamps::ParseColourDds(dds, colour, why))
                break;
            g_paintings.first.size = colour.size;
            g_paintings.first.dxt5 = colour.dxt5;
            g_paintings.dds.push_back(std::move(dds));
        }
        if (!g_paintings.dds.empty())
        {
            PaintingsMap(16, 16, false, false);
            g_paintings.over = !g_config.plotStamps[0].onFeatures.empty();
            g_paintings.multiply = g_config.plotStamps[0].textureMultiply;
            uint8_t* at = &g_paintings.cells[(_wtoi(parts[2].c_str()) * 16 + _wtoi(parts[1].c_str())) * 2];
            at[0] = static_cast<uint8_t>(g_paintings.dds.size());
            at[1] = static_cast<uint8_t>(parts.size() > 3 ? _wtoi(parts[3].c_str()) : 1);
            g_paintings.allLayers = true;
            g_paintings.detail = g_config.plotStamps[0].textureDetail;
        }
        else
            Log::Write("terrain tiles: PlotStampsTest: %s", why.c_str());
    }
    if (g_paintings.dds.empty() || g_paintings.cells.empty())
        return false;
    out.cells = g_paintings.cells.data();
    out.width = g_paintings.width;
    out.height = g_paintings.height;
    out.wrapX = g_paintings.wrapX;
    out.wrapY = g_paintings.wrapY;
    out.allLayers = g_paintings.allLayers;
    out.version = g_paintings.version;
    memcpy(out.toPlot, g_paintings.toPlot, sizeof(out.toPlot));
    out.detail = g_paintings.detail;
    out.over = g_paintings.over;
    out.multiply = g_paintings.multiply;
    return true;
}

const std::vector<std::vector<uint8_t>>& PlotStampsPaintings()
{
    return g_paintings.dds;
}

std::vector<PlotStampsSheet> PlotStampsSheets()
{
    return g_paintings.sheets;
}
