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

#define GRAPHICSUPGRADE_VERSION "0.1.0"

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
    UINT traceKey = VK_F11;     // modifiers + traceKey traces the next frame(s)
    bool keyCtrl = true, keyShift = true, keyAlt = false;  // the modifiers every hotkey needs
    int traceFrames = 1;        // frames per trace
    int autoTraceFrame = 0;     // also trace this frame number automatically (0 = off)
    bool hashTextures = true;   // content-hash lockable textures in traces
    bool clearPureDevice = true;
    // Debug: redraw every unit and ship shifted by ghostOffset (world units), so a "ghost" appears next to each one.
    bool ghostEnabled = false;
    float ghostOffset[3] = {90.0f, 0.0f, 0.0f};  // half a plot (180 units) to the east
    UINT ghostKey = 'G';
    // Reflection: ships drawn before the water are redrawn mirrored about the water plane into a texture (cleared to
    // alpha 0 each frame) that the water shader reads.
    bool reflectionEnabled = false;
    UINT reflectionSize = 512;
    float waterHeight = 49.5f;          // updated from the water draw's mtxWorld each frame; this is the start value
    UINT reflectionKey = 'R';
    bool overlayEnabled = false;        // debug: show the reflection texture (colour and alpha) in a screen corner
    UINT overlayKey = 'O';
    // Water: BtS's water drawn with the reflective, dynamic water shader and Colonization's textures, the reflection
    // and a screen-copy refraction.
    bool waterEnabled = false;
    UINT waterKey = 'W';
    // The running mod's GraphicsUpgrade folder (Mods\<name>\GraphicsUpgrade), if it has one: its GraphicsUpgrade.ini
    // overrides the game folder's setting by setting, and its water textures come first. Empty: no mod, or none.
    std::wstring modDir;  // the mod's folder (Mods\<name>), empty with no mod
    std::wstring modGraphicsDir;
    std::wstring colonizationDir = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Civilization IV Colonization";
    float waterConstants[4] = {0.22995f, 0.238738f, 0.0954951f, 0.7f};  // Colonization's f4WaterConstants
    float waterTexScale = 0.0020822511f;                              // CIV4WaterPlaneInfos.xml TextureScaling
    float waterURate = 0.01f, waterVRate = 0.002f;                    // CIV4WaterPlaneInfos.xml URate/VRate
    // Seen through the water: what ships and submarines have below the surface is drawn into the refraction, most
    // visible just under the surface and fading out at underwaterDepth (world units; 0 = off), in bands.
    float underwaterDepth = 25.0f;
    float underwaterVisibility = 0.7f;  // 0..1, just below the surface
    UINT underwaterBands = 8;
    // Sun shadows: units, ships, trees and buildings drawn from the sun into a coverage texture, blurred, and applied
    // to the ground, rivers, roads and water.
    bool shadowsEnabled = false;
    UINT shadowsKey = 'S';
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
    float waterShadow = 0.6f;         // how much of a shadow the water's surface takes (0 = none, 1 = like the ground)
    bool shadowOverlay = false;       // debug: show the shadow textures
    UINT shadowOverlayKey = 0;
    // Lighting: light colours instead of BtS's light files, per LightGroup. Colours are 0..1 here, 0..255 in the INI.
    // A colour that isn't set (no group key and no SunColour/AmbientColour) keeps the game's own.
    bool lightingEnabled = false;
    UINT lightingKey = 'L';
    UINT lightCycleKey = 'P';  // modifiers + key pauses and resumes the light cycle
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
    bool matchShadowDirection = true;  // light units and ships from the shadows' sun direction
    bool haveLightDirection = false;   // LightDirection set: light units and ships from it instead
    float lightDirection[3] = {0, 0, -1};  // direction the light travels, like SunDirection
    // What BtS baked the terrain lightmap with (its SunLight.nif); the lightmap stores it at half strength.
    float bakedTerrainSun[3] = {246 / 255.0f, 239 / 255.0f, 222 / 255.0f};
    float bakedTerrainAmbient[3] = {89 / 255.0f, 103 / 255.0f, 133 / 255.0f};
    float bakedTerrainDirection[3] = {-0.567005f, 0.433955f, -0.700134f};  // ...and its direction (light travel)
};

extern Config g_config;
extern HMODULE g_self;

void LoadConfig();
// Folder the proxy DLL was loaded from (the game folder), without a trailing backslash.
std::wstring ProxyDir();
// True if the effects should run in this game (Config::games); otherwise the proxy only passes calls through and logs.
bool EffectsAllowed();
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

// IDirect3D9 calls are rare (adapter checks, device creation), so they're all logged.
#define GU_LOG_CALL(name)                                                                                          \
    do                                                                                                             \
    {                                                                                                              \
        g_lastCall = name;                                                                                         \
        g_lastCallTick = GetTickCount();                                                                           \
        Log::Write("call: %s", name);                                                                              \
    } while (0)
