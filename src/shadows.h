// Sun shadows, the way Colonization makes them: a projected shadow texture, not a depth-compare shadow map.
//   1. Once per frame, an orthographic sun camera is placed over the area the main camera looks at (centre where
//      the view meets the ground; snapped to whole texels so shadows don't shimmer as the camera moves).
//   2. Every caster drawn into the main scene (units and animals, ships including their fixed-function parts, and
//      trees, buildings) is redrawn straight after the game's draw from the sun camera into a coverage texture,
//      using the game's own shaders: alpha (red for buildings, which have their own darkness) is written with a MAX
//      blend, so coverage = the caster's opacity.
//   3. Before Present, the coverage is blurred into the shadow texture (our shadow_blur.hlsl).
//   4. BtS draws terrain before units, so each frame's terrain is drawn with our terrain_shadow.hlsl (BtS's terrain
//      shading plus the shadow lookup) using the previous frame's shadow texture and sun matrix. Both are in world
//      space, so only moving units lag, by one frame. Rivers go through river_shadow.hlsl the same way,
//      fixed-function ground decals (roads, river foam) get one more texture stage that multiplies by the shadow,
//      and the water samples it in its own shader.
//   5. BtS's round blob shadows under units (unit_shadows.dds) are skipped while real shadows are on. Each blob also
//      marks where the next unit is, which picks out that unit's rigid fixed-function parts (weapons) as casters.
//      With building shadows on, BtS's painted building shadows (black decal textures) are skipped too.
// The sun camera's area is fitted to the ground in view each frame (up to MaxRadius), so zoomed-in shadows are sharp.
#pragma once
#include "assets.h"
#include "common.h"
#include "trace.h"

#include <functional>
#include <unordered_map>

class Shadows
{
public:
    explicit Shadows(IDirect3DDevice9* device);
    ~Shadows() { ReleaseAll(); }

    bool enabled = false;
    bool overlay = false;
    // Terrain re-light constants (Lighting::TerrainConstants), or null when GraphicsUpgrade's lighting is off. With these
    // the terrain goes through our shader even when the shadows are off.
    const float* terrainLight = nullptr;  // 8 registers (c11-c18)
    // [debug] TerrainDebug 4: the terrain drawn as this flat colour (rgb, 0-1) with its blend sheet's alpha as our
    // shader reads it (inside the tile: BlendTileClamp); w = 1: on (PS c24 of the Shader Model 3 terrain shader).
    float debugFlat[4] = {};
    // Set each frame from the lighting: the sun's travel direction now, and how dark shadows are now as a fraction
    // of their settings (the light cycle).
    const float* sunDirection = g_config.sunDirection;
    float darknessScale = 1.0f;
    // Set before each draw: the water's height as the water draw gives it (Reflection::WaterHeight; [reflection]
    // WaterHeight until the first water draw). Models standing at it don't cast; the sun camera is fitted at it.
    float waterHeight = g_config.waterHeight;
    // ...and whether that height came from a water draw (this frame or an earlier one). Then units and ships cast only
    // what's above it: the coverage has no depth, so a part under the surface (a whale's tail, a hull's keel) would
    // shade the water on its sunward side. Not before: a mod's land can lie below the start value.
    bool waterDrawn = false;
    // Set before each unit draw: the height a unit casts from (ProxyDevice::UnitGround; -1e30 if unknown). A mod's
    // land can lie above the water, and a sandworm waits under the sand: its coverage would shade the ground too.
    float unitGround = -1e30f;
    IDirect3DTexture9* terrainNormals = nullptr;  // TerrainNormals' map for per-pixel terrain lighting (s5), or null
    IDirect3DTexture9* hillShadows = nullptr;     // TerrainNormals' hill shadow mask (s6), or null
    const float* decalLight = nullptr;            // unlit ground decals' change of light (Lighting::DecalLight), or null
    float terrainGrid[2] = {};                    // world xy -> grid uv: scale, offset
    float hillGrid[3] = {};                       // world xy -> hill mask uv: scale, x offset, y offset
    // Detail normals on the terrain (s7, PS c20-c21; terrain_shadow.hlsl): the maps, and the detail texture's
    // directions on the ground (TerrainNormals::DetailAxes). Either null: none.
    class DetailNormals* detailNormals = nullptr;
    const float* detailAxes = nullptr;
    // Rivers (River_Shader P1), set each frame: the sun their ripples are lit from (its travel direction; null: the
    // game's own), and the sea's glint off them: its strength (0 = none, 128 = normal, up to 255), and for a view the
    // sky texture with the painted sun, the turn that puts it where the sun's glint is now and how bright the glint is
    // (Water::GlintSky; null texture: no glint). Either takes rivers through our shader with the shadows off too.
    const float* riverSun = nullptr;
    int riverGlint = 0;
    std::function<IDirect3DBaseTexture9*(const float* viewProj, float rot[12], float& fade)> glintSky;
    // And how far their ripples sway back and forth along the channel, in texels of the river normal map (0 = still).
    float riverShimmer = 0;
    // The ground in view, as the last sun camera fitted it: centre x, y and half-width (world units; 0 = none yet).
    float viewGround[3] = {};

    void NewFrame();  // after Present
    // Before Present: blur this frame's coverage into the texture the next frame's terrain uses. True if it did.
    bool EndFrame();
    // A fixed-function draw of BtS's blob shadow texture (BlobShadowTexture)? Also remembers where it is (see
    // RigidUnitPart).
    bool BlobShadow();
    // Drawn with one of the NoShadowTextures or a water surface's (SurfaceTextures; any stage): casts no shadow
    bool NoShadowTexture();
    // Drawn with one of [water] SurfaceTextures on stage 0: water on a model (Water::DrawSurface). The list lives here
    // because such water also casts no shadow.
    bool WaterSurface();
    // A blended fixed-function draw of one of BtS's painted shadows (building_shadow.dds, *_shadow.dds,
    // eu_an_shadow.dds, 32dropshadow.dds...): its texture is black, the shadow's shape in the alpha, and like every
    // world decal it samples the fog of war. Hidden while buildings cast real shadows, and never a caster itself.
    bool PaintedShadow();
    // A texture was just created at this address: drop what was cached about an earlier texture there (BtS frees and
    // creates textures as the view moves, and addresses get reused).
    void ForgetTexture(const void* texture)
    {
        m_blobTextures.Forget(texture);
        m_noShadowTextures.Forget(texture);
        m_surfaceTextures.Forget(texture);
        m_paintedTextures.erase(texture);
        m_mixSheetOf.erase(texture);
        for (auto& s : m_mixSheets)
            s.match.Forget(texture);
        m_stampSheetOf.erase(texture);
        for (auto& s : m_stampSheets)
            s.match.Forget(texture);
    }
    // A fixed-function mesh at the last blob shadow: part of that unit (weapons, shields)? light: the game's colours on
    // it (Lighting::GameLight), or null. BtS draws the land before the units, under lights of its own in games whose
    // units have their own. So the lights of the solid meshes before a frame's first blob are remembered, and a mesh at
    // a blob under one of them is land, once a part under another light has shown that units are lit apart. (Where the
    // units share the land's light the colours say nothing, and every mesh at a blob stays a unit's.)
    bool UnitPart(DWORD fvf, const float* light);
    // Could this vertex shader be a shadow receiver (terrain or river)? A cheap test before DrawReceiver.
    static bool MaybeReceiver(const ShaderInfo& vs);
    // Draw a BtS terrain or river draw with shadows, through our copies of its shaders. False if it doesn't apply
    // (the caller then draws it as usual).
    bool DrawReceiver(const ShaderInfo& vs, const ShaderInfo* ps, const std::function<HRESULT()>& draw, HRESULT& hr);
    // Draw a fixed-function ground decal (roads, river foam: blended, depth-tested, sampling the fog of war; not the
    // yield icons, which skip the depth test) with shadows (drawn before the units: one more texture stage multiplies
    // by the shadow texture, one more by the hill shadows) and, if it's unlit, the terrain's change of light
    // (decalLight: a stage multiplying by the texture factor). False if the draw isn't one, or there's nothing to add.
    bool DrawDecal(const std::function<HRESULT()>& draw, HRESULT& hr);
    // Right after a water draw (BtS's water constants in place): draw it again multiplying the screen by the shadow's
    // light factor at the surface ([shadows] WaterShadow), so shadows lie on the water too. True if it did.
    bool ShadeWater(const std::function<HRESULT()>& draw);
    // For receivers drawn elsewhere (the water): last frame's shadow texture (null when there's none), its matrix
    // (world -> texture, VS register layout as the terrain's c20-c23), the darkness now, and the sampler setup.
    IDirect3DTexture9* LitTexture() const { return enabled && m_havePrev ? m_lit : nullptr; }
    const float* Anchor() const { return m_haveAnchor ? m_anchor : nullptr; }  // the last blob shadow this frame, or null
    const float* LitMatrix() const { return m_prevShadowTex; }
    float Darkness() const { return g_config.shadowDarkness * darknessScale; }
    static void BindSampler(IDirect3DDevice9* dev, DWORD stage);
    // After a draw: if it was a shadow caster in the main scene, redraw it from the sun into the coverage texture.
    bool AfterDraw(const ShaderInfo* vs, bool fixedFunction, const std::function<void()>& redraw);
    void DrawOverlay();  // debug: coverage and shadow textures, second overlay row
    bool SaveBmps(const std::wstring& dir);  // debug: both textures into a trace folder

    void ReleaseDefault();  // before Reset (render targets, state blocks; made again on use)
    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }
    uint32_t FrameCasters() const { return m_frameCasters; }
    uint32_t FrameReceivers() const { return m_frameReceivers; }

    // View-projection (register form) of the main camera, from the current draw.
    bool MainViewProj(const ShaderInfo* vs, bool fixedFunction, float* vp);

private:
    bool EnsureResources();
    bool EnsureShaders();
    bool TargetIsBackBuffer();  // where the map is drawn (TargetIsMainView)
    void SetupFrame(const float* mainViewProj);
    bool SolidMesh(DWORD fvf);      // an unskinned fixed-function mesh's shape: positions, normals, writing depth
    bool RigidUnitPart(DWORD fvf);  // an unskinned fixed-function mesh of the unit whose blob was just drawn
    // World z of the ground under the tree patch being drawn (world: its World matrix); false if not known yet.
    bool TreeGround(const ShaderInfo& vs, const float* world, float& ground);
    bool BuildingPart(DWORD fvf);   // a solid lit fixed-function mesh: buildings, improvements (BuildingShadows)
    bool SamplesFogOfWar(DWORD stages);  // one of the first `stages` texture stages holds this frame's fog of war
    // The terrain draw's detail normals (s7, PS c20-c21); none unless lit (GraphicsUpgrade's lighting is on).
    void SetDetailNormals(bool lit);
    // River_Shader P1's additions (river_shadow.hlsl): its light vector from riverSun (VS c12), the camera (VS c24),
    // the glint's colour and turn (PS c12-c15) and the sky texture (s5), the ripples' sway (PS c16-c17) and the flow
    // map (s3); and the normal map (s0) read with mipmaps.
    void SetRiverConstants();
    // The flow map (src/river_flow.h), made on first use; null if it can't be.
    IDirect3DTexture9* RiverFlow();
    HRESULT DrawShadowed(IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps, const std::function<HRESULT()>& draw,
                         bool terrain);

    IDirect3DDevice9* m_dev;
    IDirect3DTexture9* m_cast = nullptr;  // this frame's coverage (alpha)
    IDirect3DSurface9* m_castSurf = nullptr;
    IDirect3DTexture9* m_lit = nullptr;   // blurred coverage, used by the next frame's terrain
    IDirect3DSurface9* m_litSurf = nullptr;
    IDirect3DStateBlock9* m_blurState = nullptr;
    IDirect3DStateBlock9* m_overlayState = nullptr;
    IDirect3DVertexShader9* m_terrainVs = nullptr;
    IDirect3DPixelShader9* m_terrainPs = nullptr;
    // The terrain in Shader Model 3 (terrain_shadow.hlsl's SM3), made on first use.
    IDirect3DVertexShader9* m_terrainSheetVs = nullptr;
    IDirect3DPixelShader9* m_terrainSheetPs = nullptr;
    bool m_terrainSheetTried = false;
    bool m_sheetPass = false;  // this terrain draw goes through that shader
    float m_sheet[4] = {};     // its c23: the sheet's size in texels, w = 1: clamp to the tile
    // [terrain tiles] MixedCoastBlend: the plots' data (s9, MixedCoastPlots, uploaded when its version changes), which
    // paired land terrain each blend sheet is (by content), and this draw's c25-c27 (c25.w = 1: weighted).
    struct MixSheet
    {
        TextureList match;
        int terrain = 0, rank = 0;
        bool overlay = false;
    };
    IDirect3DTexture9* m_mixTex = nullptr;
    unsigned m_mixVersion = 0;
    int m_mixSize[2] = {};
    std::vector<MixSheet> m_mixSheets;
    bool m_mixSheetsLoaded = false;
    std::unordered_map<const void*, int> m_mixSheetOf;  // by sheet: index in m_mixSheets, or -1
    float m_mix[12] = {};
    bool MixFor(IDirect3DBaseTexture9* sheet);  // sets m_mix (and the texture) for a terrain draw with this sheet
    // [terrain tiles] PlotStamps<n>Textures: the paintings' atlas (s11), each plot's cell (s10, PlotStampsCells, uploaded
    // when its version changes), which blend and grid sheets are the stamped layers' (by content), and this draw's
    // c28-c31 (c28.w = 1: painted).
    struct StampSheet
    {
        TextureList match;
        bool grid = false, over = false, multiply = false;
        int layer = 0;
        float detail = 0;
    };
    IDirect3DTexture9 *m_stampAtlas = nullptr, *m_stampPlots = nullptr;
    unsigned m_stampVersion = 0;
    int m_stampSize[2] = {}, m_stampCells[2] = {};
    std::vector<StampSheet> m_stampSheets;
    bool m_stampLoaded = false;
    std::unordered_map<const void*, int> m_stampSheetOf;  // by sheet: index in m_stampSheets + 1, 0 none, -1 allLayers
    float m_stamp[16] = {};
    bool StampAtlas();                            // made once from the paintings; false if it can't be
    bool StampFor(IDirect3DBaseTexture9* sheet);  // sets m_stamp (and the textures) for a terrain draw with this sheet
    bool m_loggedClamp = false;
    IDirect3DPixelShader9* m_blurPs = nullptr;
    IDirect3DVertexShader9* m_riverFloodVs = nullptr;  // River_Shader P0 (floodplain) with shadows
    IDirect3DPixelShader9* m_riverFloodPs = nullptr;
    IDirect3DVertexShader9* m_riverVs = nullptr;  // River_Shader P1 (river) with shadows
    IDirect3DPixelShader9* m_riverPs = nullptr;
    IDirect3DTexture9* m_riverFlow = nullptr;  // managed: kept over Reset
    bool m_riverFlowTried = false;
    DWORD m_riverStart = GetTickCount();
    IDirect3DVertexShader9* m_waterVs = nullptr;  // shadows on the water's surface
    IDirect3DPixelShader9* m_waterPs = nullptr;
    DWORD m_maxStages = 0;  // fixed-function texture stages usable at once (caps)
    ULONG m_internalRefs = 0;

    bool m_frameSetup = false;   // sun camera placed and coverage cleared this frame
    bool m_havePrev = false;     // m_lit holds a blurred frame
    bool m_frameFailed = false;  // the view doesn't meet the ground this frame
    bool m_haveAnchor = false;   // m_anchor = position of the last blob shadow drawn this frame
    float m_anchor[3] = {};
    float m_landLights[4][6] = {};  // diffuse, ambient of the lights on this frame's meshes before its first blob
    int m_landLightCount = 0;
    bool m_unitsLitApart = false;  // a unit's part has been seen under a light the land hasn't
    const void* m_fowTexture = nullptr;  // this frame's fog-of-war texture, from the terrain draw (compared only)
    float m_lightVP[16] = {};    // this frame's sun camera (register form)
    float m_curShadowTex[16] = {};   // world -> shadow texture for this frame's coverage
    float m_prevShadowTex[16] = {};  // ... for the texture the terrain samples (last frame's)
    uint32_t m_frameCasters = 0, m_frameReceivers = 0;
    TextureList m_blobTextures;      // BlobShadowTexture
    TextureList m_noShadowTextures;  // NoShadowTextures
    TextureList m_surfaceTextures;   // [water] SurfaceTextures: water on models casts no shadow either
    std::unordered_map<const void*, bool> m_paintedTextures;  // texture -> black (a painted shadow)
    std::unordered_map<int64_t, float> m_treeGround;  // tree patch (origin x, y) -> its ground, local z
    bool m_loggedSetup = false;
};
