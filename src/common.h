// Shared settings, logging and helpers for GraphicsUpgrade, a proxy d3d9.dll that adds reflective, dynamic water,
// sun shadows and configurable lighting to Civilization IV: Beyond the Sword.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

#define GRAPHICSUPGRADE_VERSION "1.0.4"

// What BtS lights with which light (Lighting).
enum LightGroup
{
    kLightUnit = 0,
    kLightMech = 1,
    kLightTerrain = 2,  // the baked lightmap (re-lit in terrain_shadow.hlsl)
    kLightRiver = 3,    // rivers: f3SunLightDiffuse / f3SunAmbientColor
    kLightTree = 4,     // trees: our lit copy of BtS's (unlit) tree vertex shader
    kLightBuilding = 5, // fixed-function lit meshes that aren't unit parts: buildings, improvements
    kLightWater = 6,    // the water: its sun colour (glint); no ambient
    kLightGroups = 7,
};

// Settings from GraphicsUpgrade.ini next to the proxy DLL (and a running mod's, which overrides it). The defaults here
// apply to settings the INI leaves out; developer tools are off unless the INI turns them on.
struct Config
{
    std::wstring outputDir;     // root for the log (and debug output); a subfolder per game is added
    std::wstring chainDll;      // optional d3d9.dll to forward to instead of the system one (DXVK, ReShade, ...)
    std::wstring games = L"bts";  // game ids the effects run in ("bts", "col", exe name without .exe, or "*")
    bool dumpShaders = false;   // debug: save every shader the game creates
    bool tracesEnabled = false; // debug: frame traces (hotkey, trigger file, AutoFrame)
    // Debug, [debug] TerrainDebug: BtS's terrain layers drawn opaque (1), as a flat colour each (2), or as a flat colour
    // each with its blend sheet's alpha as BtS reads it (3) or as our terrain shader does (4: BlendTileClamp). With 2-4
    // nothing else in the 3D scene is drawn and the frame is cleared to magenta, so a gap between squares shows
    // magenta. TerrainDebugKey (with KeyModifiers) steps through them.
    int terrainDebug = 0;
    UINT terrainDebugKey = '0';
    int terrainVertsPerPlot = 17;  // debug, experimental: patch BtS's terrain code for this many samples (terrain_patch.h)
    // [terrain tiles], experimental (terrain_sets.h): TilePicker=1 hooks the heightmap tile picks; TileGroups splits
    // each tile list into that many groups, a square using its corner terrains' group. TileGroupN (1..8) lists the
    // terrains of group N, here tileSetTerrains[N - 1]. Tile group 1 means every terrain not listed in another group:
    // land listed nowhere is in TileGroup1 (as well as what it lists), water listed nowhere doesn't vote.
    bool tileSetsPatch = false;
    // HillStamps=1 (experimental, hill_field.h): hill tiles cut from one map-wide field of multi-plot hill stamps
    // instead of hill1_<mask>; hooks the hill lookup even without TilePicker=1.
    bool hillField = false;
    // JigsawPiecesNTerrains=<terrain types>, N 1..8 (experimental, jigsaw.h; empty = off): the flat land and water
    // plots of those terrains are covered with the jigsaw pieces in JigsawPiecesNFolder (a folder under Assets with
    // pieces.xml and its heightmaps, or a full path), added onto the base tile of every square touching one.
    // JigsawPiecesNFloor: what those plots count as for that base tile (keep, water or land); JigsawPiecesNHeight: grid
    // units a height level (not set: pieces.xml's fHeightScale, else 1). A plot is in the first N listing its terrain.
    struct JigsawSet
    {
        std::wstring terrains, pieces;
        int base = 0;  // Jigsaw::Base: 0 keep, 1 water, 2 land
        bool hasHeight = false;
        float height = 1.0f;
    };
    JigsawSet jigsaw[8];
    // PlotStampsNTerrains=<terrain types>, N 1..8 (experimental, plot_stamps.h; empty = off): every plot of those
    // terrains gets one of the one-plot pieces in PlotStampsNFolder (as JigsawPiecesNFolder), added onto the four
    // squares round it on top of what the engine stamps there. PlotStampsNFeatures=<feature types>: the Nth feature
    // listed picks the Nth piece (no listed feature on the plot: picked by plot). PlotStampsNHideModels: textures
    // (TextureList entries) whose models aren't drawn while the stamps are on. PlotStampsNHeight: grid units a height
    // level (not set: pieces.xml's fHeightScale in world units, over the landscape's fZScale). A plot is in the first N
    // listing its terrain. PlotStampsNTextures=1: the terrain shader paints each piece's <piece>_colour.dds on the layer
    // of those terrains, which needs a blend sheet (and grid sheet) of their own (plot_stamps.h).
    // PlotStampsNTextureDetail: how much of the terrain's detail texture and its bumps shows on a painting (0 none, 1 all;
    // a value a layer). PlotStampsNOnFeatures=<feature types>, in place of Terrains: every plot with one of those features
    // gets a stamp, whatever its terrain; paintings go over the peak overlay's layer, which such a plot needs.
    // PlotStampsNTextureMultiply=1: a painting is multiplied by the light rather than added to it, so black stays black.
    // PlotStampsNFlatten=1: the pieces are weights, 0..1 over 0..65535, not heights, and level the ground under a model:
    // PlotStampsNHeight and PlotStampsNTextures don't apply. Done last in each square, each corner's in turn.
    struct PlotStampSet
    {
        std::wstring terrains, folder, features, hideModels, onFeatures;
        bool hasHeight = false;
        float height = 1.0f;
        bool textures = false;
        float textureDetail = 0;
        bool textureMultiply = false;
        bool flatten = false;
    };
    PlotStampSet plotStamps[8];
    // [debug] PlotStampsTest=<folder>;x;y[;layer] (the test harness; there's no map there): a 16 x 16 map with one stamp
    // at plot x, y, the last of <folder>\a.dds, b.dds... its painting, and every terrain draw its layer (over it if
    // PlotStamps1OnFeatures is set; a plot of another layer number isn't painted there).
    std::wstring plotStampsTest;
    int tileSets = 1;
    std::wstring tileSetTerrains[8];
    // Coast art per class (one land corner, two side by side, two diagonal, three): cells of the water's transition
    // atlas and CoastWave<class>_<NN>.nif models; more coast tiles than this reuse them in turn. Stock: 8, 8, 6, 7.
    // StandardBlendOnLand1..8 (terrain_sets.h): "TERRAIN_X: <mask 1>; ...; <mask 14>" ("tile,rotation" entries, as
    // TextureBlend): that terrain's blend on squares with no water corner, so its TextureBlend lists can be paired with
    // the coast tiles.
    std::wstring landBlends[8];
    // StandardBlendOnLand (no number): terrains taking BtS's standard blend layout on land squares
    std::wstring landBlendTerrains;
    // [terrain tiles] RiverLayerTerrain=<terrain type> (experimental, river_layer.h; empty = off): that terrain's layer
    // on every square a river runs through, its blend tile picked by the river's arms there, from RiverLayerFirstTile.
    std::wstring riverLayerTerrain;
    int riverLayerFirstTile = 33;
    // RiverLayerCaps=<folder under Assets> (river_layer.h; empty = none): shape tiles, cutting the ground to fit.
    std::wstring riverLayerCaps;
    // RiverLayerDepths=<folder under Assets> (river_layer.h): the same, but the tiles say how far to lower the ground.
    std::wstring riverLayerDepths;
    // MixedCoastBlend=1 (terrain_sets.h, BlendSquare): on a coast square of two or more land terrains with paired lists,
    // each paired land layer paints the whole land and the terrain shader weights it by its share of the square's land
    // corners, so they blend across the square instead of meeting in square blocks. Needs our terrain shader (the
    // lighting or the shadows on).
    bool mixedCoastBlend = true;
    int coastAtlasCells[4] = {8, 8, 6, 7};
    int coastWaveModels[4] = {8, 8, 6, 7};
    float waterTime = -1;       // debug: the water's clock stopped at this many seconds (< 0: running)
    UINT traceKey = '9';     // modifiers + traceKey traces the next frame(s)
    bool keyCtrl = true, keyShift = true, keyAlt = false;  // the modifiers every hotkey needs
    UINT compareKey = VK_OEM_2;  // modifiers + compareKey saves a GraphicsUpgrade on/off pair of screenshots
    int traceFrames = 1;        // frames per trace
    int autoTraceFrame = 0;     // also trace this frame number automatically (0 = off)
    bool hashTextures = true;   // content-hash lockable textures in traces
    bool clearPureDevice = true;
    // Debug: redraw every unit and ship shifted by ghostOffset (world units), so a "ghost" appears next to each one.
    bool ghostEnabled = false;
    float ghostOffset[3] = {90.0f, 0.0f, 0.0f};  // half a plot (180 units) to the east
    UINT ghostKey = '8';
    // Reflection: ships drawn before the water are redrawn mirrored about the water plane into a texture (cleared to
    // alpha 0 each frame) that the water shader reads.
    bool reflectionEnabled = false;
    UINT reflectionSize = 512;
    float waterHeight = 49.5f;          // the start value: the water draw's mtxWorld gives the real one (Reflection)
    UINT reflectionKey = 0;
    bool overlayEnabled = false;        // debug: show the reflection texture (colour and alpha) in a screen corner
    UINT overlayKey = 0;
    // Water: BtS's water drawn with the reflective, dynamic water shader and Colonization's textures, the reflection
    // and a screen-copy refraction.
    bool waterEnabled = false;
    UINT waterKey = '1';
    // The water's normal maps: NormalMapToggleKey cycles water_normal.dds, water_normal_1.dds, _2...; NormalMap is
    // the one at the start (0 = water_normal.dds).
    UINT waterNormalKey = '7';
    int waterNormalMap = 0;
    // The running mod's GraphicsUpgrade folder (Mods\<name>\GraphicsUpgrade), if it has one: its GraphicsUpgrade.ini
    // overrides the game folder's setting by setting, and its water textures come first. Empty: no mod, or none.
    std::wstring modDir;  // the mod's folder (Mods\<name>), empty with no mod
    std::wstring modGraphicsDir;
    // Water on models (an oasis' pool): fixed-function draws with one of these textures (TextureList entries) get the
    // water's ripples, sky reflection and sun glint on top, and cast no shadow.
    std::wstring waterSurfaceTextures;
    float waterConstants[4] = {0.22995f, 0.238738f, 0.0954951f, 0.7f};  // the water shader's f4WaterConstants
    float waterTexScale = 0.0020822511f;                              // CIV4WaterPlaneInfos.xml TextureScaling
    float waterURate = 0.01f, waterVRate = 0.002f;                    // CIV4WaterPlaneInfos.xml URate/VRate
    // Seen through the water: what ships and submarines have below the surface is drawn into the refraction, most
    // visible just under the surface and fading out at underwaterDepth (world units; 0 = off), in bands.
    float underwaterDepth = 25.0f;
    float underwaterVisibility = 0.7f;  // 0..1, just below the surface
    // Shoreline and shallow water (Shader Model 3): the sun's glint follows the sun; sunlight shimmering on the sea
    // floor near the coast (caustics), a strength (0 = off). Their textures come from the
    // GraphicsUpgrade folder.
    int glintFollowsSun = 1;  // 0 = a fixed glint, 1 = follows the sun in view, 2 = exactly where it is
    float caustics = 1.0f;
    int waveShading = 64;  // the waves' faces lit by the glint's light, and sparkle around it: 0 (off) to 255
    float foamLight = 0.4f;  // how much of the change of light the coast waves' foam takes: 0 = white, as BtS, to 1
    int riverGlint = 128;  // the sea's glint off the rivers: 0 (off), 128 (normal) to 255
    float riverShimmer = 3;  // how far the rivers' ripples sway along the channel, texels of their normal map (0 = still)
    UINT underwaterBands = 8;
    // Sun shadows: units, ships, trees and buildings drawn from the sun into a coverage texture, blurred, and applied
    // to the ground, rivers, roads and water.
    bool shadowsEnabled = false;
    UINT shadowsKey = '2';
    UINT shadowSize = 2048;           // shadow texture size in pixels
    float shadowMaxRadius = 2000.0f;  // the shadowed area fits the ground in view, up to this half-width (plot = 180)
    float shadowSoftness = 0.5f;      // blur tap offset in shadow texels (0 = hard edges)
    float sunDirection[3] = {-0.567005f, 0.433955f, -0.700134f};  // the way the sunlight travels
    float shadowDarkness = 0.45f;     // fraction of light removed in full shadow
    bool hideBlobShadows = true;      // drop BtS's round blob shadows under units
    std::wstring blobShadowTexture;   // the blob shadows' texture (TextureList entries)
    std::wstring noShadowTextures;    // textures whose models cast no shadows (TextureList entries)
    bool buildingShadows = true;      // buildings and other solid fixed-function meshes cast shadows too
    float buildingDarkness = 0.68f;   // their shadows' darkness (BtS's painted building shadows: about 0.67)
    bool hillShadows = true;          // hills and peaks shade the terrain behind them (from the terrain's heights)
    float hillShadowSoftness = 8.0f;  // world units the terrain must rise above the sunbeam for full shadow
    float hillShadowLength = 1.64f;    // hill shadows' length as a multiple of the terrain's true heights
    float waterShadow = 0.6f;         // how much of a shadow the water's surface takes (0 = none, 1 = like the ground)
    bool shadowOverlay = false;       // debug: show the shadow textures
    UINT shadowOverlayKey = 0;
    // Lighting: light colours instead of BtS's light files, per LightGroup. Colours are 0..1 here, 0..255 in the INI.
    // A colour that isn't set (no group key and no SunColour/AmbientColour) keeps the game's own.
    bool lightingEnabled = false;
    UINT lightingKey = '3';
    // Anisotropic filtering of the map's textures: up to this many samples along the view (1 = BtS's own filtering),
    // capped at what the graphics card supports.
    UINT anisotropy = 8;
    UINT anisotropyKey = '5';
    // Detail normals (DetailNormals): small bumps on the ground, lit by the sun as it is now (needs the lighting on).
    // From a normal map shipped beside a terrain's detail texture (x detailNormalStrength), or else from the detail
    // texture's brightness read as height (x detailBumpiness; 0 = shipped maps only).
    bool detailNormals = true;
    UINT detailNormalsKey = '6';  // modifiers + key switches them on and off in game
    float detailNormalStrength = 1.0f;
    float detailBumpiness = 1.5f;
    bool blendTileClamp = true;  // read blend sheets inside each square's tile at every mip (Shader Model 3)
    // Models drawn with one of these textures (TextureList entries) show under the culture borders' tint, as the
    // ground does, rather than over it (BtS draws the borders first, then the models over them).
    std::wstring modelsUnderBorders;
    // BtS's area borders (the settler's city-site guide) drawn with one of these textures (TextureList entries) are laid
    // over the terrain's heights instead of flat beneath the hills (AreaBorders).
    std::wstring areaBorderTextures;
    UINT lightCycleKey = '4';  // modifiers + key pauses and resumes the light cycle
    float lightSun[kLightGroups][3] = {};
    float lightAmbient[kLightGroups][3] = {};
    bool lightSunSet[kLightGroups] = {}, lightAmbientSet[kLightGroups] = {};
    // The group's own *Sun / *Ambient key is set (it then keeps that colour through the light cycle).
    bool lightSunOwn[kLightGroups] = {}, lightAmbientOwn[kLightGroups] = {};
    // Light cycle: the shared sun and ambient colours (every group without its own *Sun / *Ambient key) and the sun's
    // direction follow keyframes over time.
    struct LightCycleKey
    {
        float step;      // position in the cycle, 0 .. lightCycleStepCount
        float value[3];  // a colour (0..1), a direction (unit, the way the light travels) or azimuth, elevation (degrees)
    };
    bool lightCycleOn = false;
    float lightCycleTime = 600.0f;     // seconds for one full cycle
    float lightCycleStepCount = 24.0f; // positions in a cycle (e.g. 24 hours)
    float lightCycleStart = 12.0f;     // where the cycle is when the game starts (12 = midday)
    std::vector<LightCycleKey> lightCycle;         // sun, sorted by step
    std::vector<LightCycleKey> lightCycleAmbient;  // ambient (empty: the ambient doesn't cycle)
    std::vector<LightCycleKey> lightCycleDirection;  // the sun's (and moon's) direction (empty: [shadows] SunDirection)
    // ...or where it is in the sky: azimuth (degrees anticlockwise from east, the side the light comes from) and
    // elevation (degrees above the horizon), blended as angles so the sun goes round rather than back. Wins over
    // lightCycleDirection.
    std::vector<LightCycleKey> lightCycleAngles;
    // ...or the simple sun path: once round the compass per cycle at a steady speed, clockwise like the real sun,
    // from sunMiddayFrom (a compass bearing: 0 north, 90 east) at midday; up to sunMiddayHeight at midday and
    // moonHeight at midnight, down to sunriseHeight at sunrise and sunset (degrees above the horizon). Wins over the
    // lists.
    bool sunPath = false;
    float sunMiddayFrom = 157.5f, sunMiddayHeight = 45.0f, sunriseHeight = 30.0f, moonHeight = 50.0f;
    bool shadowsFollowSun = true;  // shadows fade as the cycle's sun dims (darkness x sun brightness / brightest)
    // The moon's glint on the water (0..1): while the light cycle runs, the water's sun turns to this from 20 to 4 h
    // (a sixth of the cycle either side of midnight; full at midnight). Not set: the cycle's colour throughout.
    bool moonGlintSet = false;
    float moonGlint[3] = {1, 1, 1};
    // Effects that take the light: blended, unlit fixed-function draws in the map (particles: dust, smoke, a
    // sandstorm) drawn with one of these textures (TextureList entries) change with the light as the ground under them
    // does (Lighting::DrawEffect). litEffectsAll: the INI says "*", every such draw.
    std::wstring litEffectTextures;
    bool litEffectsAll = false;
    // [lighting] GroundLitTextures: fixed-function models with one of these on stage 0 add the light to their texture
    // as the terrain does, rather than multiplying by it (Lighting::Headroom). TextureList entries.
    std::wstring groundLitTextures;
    // How bright the lights can make buildings, improvements and resources (the Building group's fixed-function
    // draws), in multiples of their textures' colours: 1 = BtS's (Direct3D's lighting stops at 1), up to 2 = as the
    // ground (BtS bakes the terrain's light at half strength and doubles it). When sun and ambient add up past 1,
    // whatever faces up stays at its texture's colour whatever the light, unless this is above 1 (Lighting::Headroom).
    float buildingLightRange = 1.0f;
    bool matchShadowDirection = true;  // light units and ships from the shadows' sun direction
    bool haveLightDirection = false;   // LightDirection set: light units and ships from it instead
    float lightDirection[3] = {0, 0, -1};  // direction the light travels, like SunDirection
    // What the game baked the terrain lightmap with (its SunLight.nif); the lightmap stores it at half strength. These
    // are BtS's own; LoadBakedTerrainLight replaces the ones the INI doesn't set (*Set) with the running game's.
    float bakedTerrainSun[3] = {246 / 255.0f, 239 / 255.0f, 222 / 255.0f};
    float bakedTerrainAmbient[3] = {89 / 255.0f, 103 / 255.0f, 133 / 255.0f};
    float bakedTerrainDirection[3] = {-0.567005f, 0.433955f, -0.700134f};  // ...and its direction (light travel)
    bool bakedTerrainSunSet = false, bakedTerrainAmbientSet = false, bakedTerrainDirectionSet = false;
};

extern Config g_config;
extern HMODULE g_self;

void LoadConfig();
// [debug] TerrainVertsPerPlot: patch the running exe's terrain code (terrain_patch.cpp); logs what it did. Once.
void ApplyTerrainPatch();
// [terrain tiles] TilePicker: hook the heightmap tile picks (terrain_sets.cpp); logs what it did. Once.
void ApplyTileSets();
// [terrain tiles] PlotStamps<n>Flatten: wraps the calls to the peak stamp if any is set (terrain_sets.cpp); after every
// other patch of those calls, whose target it calls. Once.
void ApplyPlotFlatten();
// [terrain tiles] StandardBlendOnLand(1..8): hooks the terrain layers' blend pick if any is set (terrain_sets.cpp).
// Once.
void ApplyLandBlends();
// [terrain tiles] RiverLayerTerrain: hooks the terrain layer records if set (terrain_sets.cpp). Once.
void ApplyRiverLayer();
// How much of the canyons' cut to add back at a world position, in world units (0 where nothing was cut; the whole cut
// in a shallow trench, none in a deep gorge through a mesa: RiverLayer::Cuts::Bridge), and a number that changes
// whenever any cut does (terrain_sets.cpp): the height texture adds it back, so borders bridge the trenches.
float RiverCutAt(float wx, float wy);
unsigned RiverCutVersion();
float RiverCutRawAt(float wx, float wy);  // debug: the whole cut there, bridged or not, in world units
std::string RiverCutDescribe();           // debug: RiverLayer::Cuts::Describe and the z scale, JSON fields
std::string LandBlendsSummary();  // "" if nothing new since the last call
// "terrain tile groups: ..." (picks by tile group, hill/peak variants used) when they changed since the last call,
// else "".
std::string TileSetsSummary();
// [terrain tiles] PlotStamps1..8: true once a set's stamps are read and going on the map (terrain_sets.cpp); every set's
// HideModels entries, joined with ';' (TextureList entries).
bool PlotStampsOn();
std::wstring PlotStampsHiddenModels();
// [terrain tiles] MixedCoastBlend (terrain_sets.cpp): the plots the terrain shader weights blended squares by (false
// when off): rgba a plot (r = terrain + 1, g = 255 where water, 128 on a peak plot, b = the paired land terrain's rank by
// layer order, a = 255 where the square with this plot at its south-west corner blends), the map's size and wrap, a
// version that changes with the data, and world xy -> plot coordinates (x = scale, yz = offset; plot centres at whole
// numbers). And the paired land terrains' blend and grid sheets (paths under Assets), each with its terrain + 1 and rank.
struct MixedCoastMap
{
    const uint8_t* rgba = nullptr;
    int width = 0, height = 0;
    bool wrapX = false, wrapY = false;
    unsigned version = 0;
    float toPlot[3] = {};
};
bool MixedCoastPlots(MixedCoastMap& out);
struct MixedCoastSheet
{
    std::wstring path;
    int terrain = 0, rank = 0;
    bool overlay = false;  // the peak layer: its share is of all the land corners
};
std::vector<MixedCoastSheet> MixedCoastSheets();
// [terrain tiles] PlotStamps<n>Textures (terrain_sets.cpp): which painting each plot has, for the terrain shader (false
// when none): two bytes a plot (the first 0: no stamp, 255: a stamp with no painting, else the atlas cell + 1; the second
// the layer it's painted on, PlotStampsSheet::layer), the map's size and wrap, a version that changes with the data, and
// world xy -> plot coordinates (x = scale, yz = offset; plot centres at whole numbers). The paintings' DDS files, one a
// cell, and the blend and grid sheets of the layers they're painted on (paths under Assets, or a full path), each with
// its layer number, whether paintings go over the layer's own blend (else they replace it where any shows), whether
// they're multiplied by the light, and how much detail texture shows on them. allLayers: [debug] PlotStampsTest, every
// terrain draw, as over, multiply and detail say.
struct PlotStampsMap
{
    const uint8_t* cells = nullptr;
    int width = 0, height = 0;
    bool wrapX = false, wrapY = false, allLayers = false;
    unsigned version = 0;
    float toPlot[3] = {};
    float detail = 0;
    bool over = false, multiply = false;
};
bool PlotStampsCells(PlotStampsMap& out);
struct PlotStampsSheet
{
    std::wstring path;
    bool grid = false;
    int layer = 0;
    bool over = false, multiply = false;
    float detail = 0;
};
const std::vector<std::vector<uint8_t>>& PlotStampsPaintings();
std::vector<PlotStampsSheet> PlotStampsSheets();
// The Baked* light the INI leaves out, from the SunLight.nif the game uses (the running mod's first); logs it. Once.
void LoadBakedTerrainLight();
// Folder the proxy DLL was loaded from (the game folder), without a trailing backslash.
std::wstring ProxyDir();
// True if the effects should run in this game (Config::games); otherwise the proxy only passes calls through and logs.
bool EffectsAllowed();
// True when render target 0 is where the map is drawn: the swap chain's back buffer, or a render target the size of
// the screen (as the Civilopedia opens, BtS draws the map once into one, to show behind it). The interface's 3D views
// (the unit in the lower-left corner, its flag, the minimap, the Civilopedia's models) go into small textures of their
// own and don't count.
bool TargetIsMainView(IDirect3DDevice9* dev);
// While one of the game's draws is handled (ProxyDevice::Dispatch): TargetIsMainView worked out once, at the start,
// and answered from that for the rest of the draw (GraphicsUpgrade's own passes put back any render target they
// switch to before going on). Nests.
class MainViewScope
{
public:
    explicit MainViewScope(IDirect3DDevice9* dev);
    ~MainViewScope();
    bool Main() const { return m_main; }

private:
    IDirect3DDevice9* m_prevDev;
    bool m_prevMain, m_main;
};
// A hotkey's name as the INI writes it (W, F11, PrintScreen, 0x..., none).
std::string KeyName(UINT vk);

// 64-bit FNV-1a, used for shader, texture and constant-block identity.
uint64_t Fnv1a(const void* data, size_t size, uint64_t hash = 1469598103934665603ull);
std::string Hex64(uint64_t value);
std::string Ptr(const void* p);
std::string Guid(REFIID id);
std::wstring Widen(const std::string& s);
bool EnsureDir(const std::wstring& path);
bool WriteBytes(const std::wstring& path, const void* data, size_t size);

// Session log: <OutputDir>\<game>\session.log, one timestamped line per call.
namespace Log
{
void Init();
void Write(const char* fmt, ...);
const std::wstring& GameDir();
const std::string& GameId();
}

// Gamebryo's fixed-function skinned meshes (BtS ship rigging and hull parts, FVF 0x11a = XYZB3|NORMAL|TEX1) carry
// blend weights and normals. Unlit decals drawn while vertex blending is still on (the unit selection circle,
// FVF 0x10a = XYZB3|TEX1) have no normals, so they aren't treated as units. FVF 0 = a vertex declaration is in use;
// accepted, as it can't be told apart here.
inline bool SkinnedLitFvf(DWORD fvf)
{
    if (fvf == 0)
        return true;
    DWORD pos = fvf & D3DFVF_POSITION_MASK;
    return pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5 && (fvf & D3DFVF_NORMAL) != 0;
}

// Current reference count of the device (our own objects hold references too; see ProxyDevice::Release).
inline ULONG DeviceRefs(IDirect3DDevice9* dev)
{
    dev->AddRef();
    return dev->Release();
}

// Render target 0, depth surface and viewport, saved and restored around our own passes.
struct SavedTarget
{
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* depth = nullptr;
    D3DVIEWPORT9 viewport = {};

    void Save(IDirect3DDevice9* dev)
    {
        dev->GetRenderTarget(0, &rt);
        dev->GetDepthStencilSurface(&depth);
        dev->GetViewport(&viewport);
    }
    void Restore(IDirect3DDevice9* dev)
    {
        dev->SetRenderTarget(0, rt);  // also resets the viewport, hence restoring it last
        dev->SetDepthStencilSurface(depth);
        dev->SetViewport(&viewport);
        if (rt)
            rt->Release();
        if (depth)
            depth->Release();
        rt = depth = nullptr;
    }
};

// Last Direct3D call made through the proxy and when, for the stall watchdog (StartWatchdog in util.cpp).
extern const char* volatile g_lastCall;
extern volatile DWORD g_lastCallTick;
void StartWatchdog();

// Records every call for the watchdog and logs the first call of each method (one line per method per process).
// The game checks formats thousands of times at startup (IDirect3D9::CheckDeviceFormat), so even IDirect3D9's calls
// are logged only the first time.
#define GU_FIRST_CALL(name)                                                                                        \
    do                                                                                                             \
    {                                                                                                              \
        g_lastCall = name;                                                                                         \
        g_lastCallTick = GetTickCount();                                                                           \
        static bool s_called = false;                                                                              \
        if (!s_called)                                                                                             \
        {                                                                                                          \
            s_called = true;                                                                                       \
            Log::Write("first call: %s", name);                                                                    \
        }                                                                                                          \
    } while (0)

