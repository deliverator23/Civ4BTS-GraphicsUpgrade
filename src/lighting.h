// Light colours from GraphicsUpgrade.ini instead of BtS's light files (SunLight.nif, UnitLight.nif, mechlight.nif,
// TerrainLight.nif). BtS hands its light colours to the shaders as named constants, so they are swapped per draw:
//   units (TCiv4Skinning*)         f3UnitLightDiffuse, f3UnitAmbientColor, f3UnitLightDir
//   mech units and ships (Civ4Mech) f3MechLightDiffuse, f3MechAmbientColor, f3MechLightDir
//   rivers (River_Shader P1)        f3SunLightDiffuse, f3SunAmbientColor (BtS: SunLight.nif)
// The terrain's light is baked into a lightmap when a map loads; our terrain shader (terrain_shadow.hlsl) re-lights
// it. BtS bakes texel = (ambient + sun x amount) / 2 with its SunLight colours, so amount = (2 x texel - baked
// ambient) / baked sun, then the texel becomes (INI ambient + INI sun x amount) / 2. With the terrain colours equal
// to the baked ones the lightmap is used exactly as baked.
// Units and ships can be lit from the shadows' sun direction (so their shading matches their shadows) or from an INI
// direction.
// Trees: BtS's tree vertex shader is unlit (colour 1); tree draws get our copy with lighting (tree_lit.hlsl).
// Fixed-function lit draws (buildings, improvements, ships' fixed-function parts, units' rigid parts) get the
// group's colours through Direct3D's lights, the way BtS sets them (its D3DRS_AMBIENT is white and each light
// carries its own ambient): each enabled directional light's diffuse and specular = sun, its ambient = ambient, and
// its direction. A colour left unset keeps the game's own.
#pragma once
#include "common.h"
#include "trace.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

class Lighting
{
public:
    explicit Lighting(IDirect3DDevice9* device) : m_dev(device) {}
    ~Lighting() { ReleaseAll(); }

    bool enabled = false;

    // Before a draw: put the INI colours into the shaders' light constants, the tree shader or the fixed-function
    // lights (remembering the game's). ffGroup: the group of a fixed-function draw (LightGroup), or -1.
    void Apply(const ShaderInfo* vs, const ShaderInfo* ps, int ffGroup);
    // After the draw (and its redraws): the game's values back.
    void Restore();
    // Pixel shader constants c11-c18 for terrain_shadow.hlsl: baked ambient, 1 / baked sun, ambient, sun,
    // (1 = re-light), towards the new sun, towards the baked sun, normal map (scale, offset, on). normalSpacing: the
    // terrain normal map's world units per texel (0 = no normal map), normalCells its size.
    void TerrainConstants(float* out32, float normalSpacing, UINT normalCells) const;
    // Once per lightmap texture: log how well BtS's bake matches "ambient + sun x amount" (the re-light's assumption).
    void CheckLightmap(IDirect3DBaseTexture9* lightmap);
    void ReleaseAll();  // our tree shader
    // A light slot the game has enabled (any index: BtS uses 9 and up). The fixed-function override looks at these.
    void NoteLightSlot(DWORD index)
    {
        if (std::find(m_slots.begin(), m_slots.end(), index) == m_slots.end())
            m_slots.push_back(index);
    }
    // Once per frame: where the light cycle is now (LightCycleOn).
    void NewFrame();
    const float* CycleSun() const { return m_cycleSun; }
    const float* CycleAmbient() const { return m_cycleAmbient; }
    std::string Describe() const;  // the cycle's colours now, for the log
    // Set each frame: sun shadows are on, so BtS's painted tree shadows go (our tree vertex shader drops them, with
    // BtS's own unlit colours when the lighting is off).
    bool hidePaintedTreeShadows = false;
    bool cyclePaused = false;  // the light cycle stands still (LightCycleToggleKey)
    // The map's terrain was drawn this frame. The cycle's clock only moves on through frames that show the world, so
    // menus and loading screens don't use up the day (the game opens at LightCycleStartStep).
    void WorldDrawn() { m_worldThisFrame = true; }
    // For the log: whether the cycle is being applied, and where it is.
    bool CycleActive() const { return m_cycleActive; }
    double CyclePos() const { return m_cyclePos; }
    // The sun's travel direction now: the cycle's (sun path, angles or directions) or [shadows] SunDirection.
    // Shadows, hill shadows and everything lit "from the shadows' sun" use it.
    const float* SunDirection() const { return m_cycleDirActive ? m_cycleDir : g_config.sunDirection; }
    // How dark shadows are now, as a fraction of their settings: the cycle's sun brightness over its brightest
    // (ShadowsFollowSun), else 1.
    float ShadowScale() const { return m_shadowScale; }
    // The tint of the sky reflected in the water: the ambient now (the terrain's: AmbientColour, the light cycle's,
    // or TerrainAmbient) over the light cycle's own midday ambient while the cycle sets it, else over Colonization's
    // own (89,103,133); squared (the water works in squared colours); 1 with the lighting off. So the reflected sky
    // follows the light: as it is at the cycle's midday, darker at night, green under a green ambient.
    const float* SkyLight();
    // The water's sun now (for its glint): the Water group's, turning to MoonGlint at night while the light cycle runs.
    // Null: none set (the water keeps Colonization's own).
    const float* WaterSun();
    ULONG InternalRefs() const { return m_internalRefs; }

    // A group's sun / ambient colour: the cycle's for groups without their own *Sun / *Ambient key while it runs,
    // else the INI's.
    bool CyclesSun(int g) const { return m_cycleActive && !g_config.lightCycle.empty() && !g_config.lightSunOwn[g]; }
    bool CyclesAmbient(int g) const
    {
        return m_cycleActive && !g_config.lightCycleAmbient.empty() && !g_config.lightAmbientOwn[g];
    }
    bool SunSet(int g) const { return g_config.lightSunSet[g] || CyclesSun(g); }
    const float* Sun(int g) const { return CyclesSun(g) ? m_cycleSun : g_config.lightSun[g]; }
    bool AmbientSet(int g) const { return g_config.lightAmbientSet[g] || CyclesAmbient(g); }
    const float* Ambient(int g) const { return CyclesAmbient(g) ? m_cycleAmbient : g_config.lightAmbient[g]; }

private:
    void Set(bool vs, int reg, const float* rgb);
    bool Direction(int group, float* dir) const;  // false = keep the game's direction
    bool EnsureTreeShader();
    void ApplyFixedFunction(int group);

    struct Saved
    {
        bool vs;
        UINT reg;
        float value[4];
    };
    IDirect3DDevice9* m_dev;
    Saved m_saved[8] = {};
    int m_count = 0;
    IDirect3DVertexShader9* m_treeVs = nullptr;  // tree_lit.hlsl
    IDirect3DVertexShader9* m_gameVs = nullptr;  // the game's tree shader while ours is bound (one reference)
    ULONG m_internalRefs = 0;
    bool m_treeFailed = false;
    struct SavedLight
    {
        DWORD index;
        D3DLIGHT9 light;
    };
    SavedLight m_lights[8] = {};
    int m_lightCount = 0;
    std::vector<DWORD> m_slots;  // light slots the game has enabled
    bool m_cycleActive = false;
    float m_cycleSun[3] = {}, m_cycleAmbient[3] = {}, m_cycleDir[3] = {};
    bool m_cycleDirActive = false;
    float m_shadowScale = 1.0f;
    float m_brightestSun = -1;  // the brightest sun keyframe's brightness (computed once)
    float m_skyLight[3] = {1, 1, 1};
    float m_waterSun[3] = {};
    LARGE_INTEGER m_last = {}, m_frequency = {};
    double m_cycleSeconds = 0;  // time the cycle has run (it stands still while paused or with the lighting off)
    bool m_worldThisFrame = false;
    bool m_loggedCycle = false;
    int m_loggedStep = -1;   // the last whole step logged
    double m_cyclePos = 0;   // where the cycle is, 0 .. step count
    std::unordered_set<const void*> m_checked;
};
