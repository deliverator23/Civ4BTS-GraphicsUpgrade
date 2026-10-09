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
    // Set each frame from the lighting: the sun's travel direction now, and how dark shadows are now as a fraction
    // of their settings (the light cycle).
    const float* sunDirection = g_config.sunDirection;
    float darknessScale = 1.0f;
    IDirect3DTexture9* terrainNormals = nullptr;  // TerrainNormals' map for per-pixel terrain lighting (s5), or null
    IDirect3DTexture9* hillShadows = nullptr;     // TerrainNormals' hill shadow mask (s6), or null
    float terrainGrid[2] = {};                    // world xy -> grid uv: scale, offset

    void NewFrame();  // after Present
    void EndFrame();  // before Present: blur this frame's coverage into the texture the next frame's terrain uses
    // A fixed-function draw of BtS's blob shadow texture (BlobShadowTexture)? Also remembers where it is (see
    // RigidUnitPart).
    bool BlobShadow();
    bool NoShadowTexture();  // drawn with one of the NoShadowTextures (stage 0): casts no shadow
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
        m_paintedTextures.erase(texture);
    }
    // A fixed-function mesh at the last blob shadow: part of that unit (weapons, shields)?
    bool UnitPart(DWORD fvf) { return RigidUnitPart(fvf); }
    // Could this vertex shader be a shadow receiver (terrain or river)? A cheap test before DrawReceiver.
    static bool MaybeReceiver(const ShaderInfo& vs);
    // Draw a BtS terrain or river draw with shadows, through our copies of its shaders. False if it doesn't apply
    // (the caller then draws it as usual).
    bool DrawReceiver(const ShaderInfo& vs, const ShaderInfo* ps, const std::function<HRESULT()>& draw, HRESULT& hr);
    // Draw a fixed-function ground decal (roads, river foam: blended, sampling the fog of war, drawn before the units)
    // with shadows: one more texture stage multiplies by the shadow texture. False if the draw isn't one.
    bool DrawDecal(const std::function<HRESULT()>& draw, HRESULT& hr);
    // Right after a water draw (BtS's water constants in place): draw it again multiplying the screen by the shadow's
    // light factor at the surface ([shadows] WaterShadow), so shadows lie on the water too. True if it did.
    bool ShadeWater(const std::function<HRESULT()>& draw);
    // For receivers drawn elsewhere (the water): last frame's shadow texture (null when there's none), its matrix
    // (world -> texture, VS register layout as the terrain's c20-c23), the darkness now, and the sampler setup.
    IDirect3DTexture9* LitTexture() const { return enabled && m_havePrev ? m_lit : nullptr; }
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

private:
    bool EnsureResources();
    bool EnsureShaders();
    bool TargetIsBackBuffer();
    // View-projection (register form) of the main camera, from the current draw.
    bool MainViewProj(const ShaderInfo* vs, bool fixedFunction, float* vp);
    void SetupFrame(const float* mainViewProj);
    bool RigidUnitPart(DWORD fvf);  // an unskinned fixed-function mesh of the unit whose blob was just drawn
    bool BuildingPart(DWORD fvf);   // a solid lit fixed-function mesh: buildings, improvements (BuildingShadows)
    bool SamplesFogOfWar(DWORD stages);  // one of the first `stages` texture stages holds this frame's fog of war
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
    IDirect3DPixelShader9* m_blurPs = nullptr;
    IDirect3DVertexShader9* m_riverFloodVs = nullptr;  // River_Shader P0 (floodplain) with shadows
    IDirect3DPixelShader9* m_riverFloodPs = nullptr;
    IDirect3DVertexShader9* m_riverVs = nullptr;  // River_Shader P1 (river) with shadows
    IDirect3DPixelShader9* m_riverPs = nullptr;
    IDirect3DVertexShader9* m_waterVs = nullptr;  // shadows on the water's surface
    IDirect3DPixelShader9* m_waterPs = nullptr;
    DWORD m_maxStages = 0;  // fixed-function texture stages usable at once (caps)
    ULONG m_internalRefs = 0;

    bool m_frameSetup = false;   // sun camera placed and coverage cleared this frame
    bool m_havePrev = false;     // m_lit holds a blurred frame
    bool m_frameFailed = false;  // the view doesn't meet the ground this frame
    bool m_haveAnchor = false;   // m_anchor = position of the last blob shadow drawn this frame
    float m_anchor[3] = {};
    const void* m_fowTexture = nullptr;  // this frame's fog-of-war texture, from the terrain draw (compared only)
    float m_lightVP[16] = {};    // this frame's sun camera (register form)
    float m_curShadowTex[16] = {};   // world -> shadow texture for this frame's coverage
    float m_prevShadowTex[16] = {};  // ... for the texture the terrain samples (last frame's)
    uint32_t m_frameCasters = 0, m_frameReceivers = 0;
    TextureList m_blobTextures;      // BlobShadowTexture
    TextureList m_noShadowTextures;  // NoShadowTextures
    std::unordered_map<const void*, bool> m_paintedTextures;  // texture -> black (a painted shadow)
    bool m_loggedSetup = false;
};
