// BtS's water drawn with the reflective, dynamic water shader (shaders\water.hlsl, reverse engineered from
// Colonization's), which also takes sun shadows on its surface and the light cycle's sky light.
//
// Both games' water vertex shaders take the same inputs (position, TEXCOORD1 = coast fade, TEXCOORD2 = fog of war), so
// BtS's water geometry is used as is. The constants are derived from BtS's own water draw:
//   mtxWorldViewProj, mtxWorld   copied from BtS's water constants
//   mtxInvView row 3 (eye)       solved from the view-projection (clip x = y = w = 0 at the camera)
//   mtxReflection                texture projection of the mirrored view-projection (as the reflection pass)
//   mtxRefraction                texture projection of the view-projection (a screen-space lookup)
//   mtxWaterTexture1/2           normal-map scale and scroll from Colonization's CIV4WaterPlaneInfos.xml
// Textures: Colonization's normal map and environment cube (GraphicsUpgrade\water_normal.dds and water_env.dds, or
// read from a Colonization install's Art0.FPK; loaded with the game's own d3dx9_33.dll), BtS's coast fade and fog of war,
// the reflection, and a refraction texture copied from the back buffer just before the ships are drawn (BtS has drawn
// the sea floor by then).
#pragma once
#include "common.h"
#include "trace.h"

#include <functional>
#include <vector>

class Water
{
public:
    explicit Water(IDirect3DDevice9* device) : m_dev(device) {}
    ~Water() { ReleaseAll(); }

    bool enabled = false;
    // The water's sun colour (0..1) from the lighting, set each frame; null: Colonization's own.
    const float* sunColour = nullptr;
    // Set before each water draw: the sun shadows (texture null: none; matrix as the terrain's; darkness now) and the
    // sky light (the reflected sky's tint, squared colours).
    IDirect3DTexture9* shadowTexture = nullptr;
    const float* shadowMatrix = nullptr;
    float shadowDarkness = 0;
    const float* skyLight = nullptr;
    // Shoreline and shallow water (water.hlsl built with SHORE): set before each water draw. The sun's travel
    // direction (the glint follows it), and the shore texture (TerrainNormals::ShoreTexture; null: no caustics or
    // the caustics) with world xy -> its uv (scale, offset).
    const float* sunDirection = nullptr;
    // Where the light cycle is in its day: 0 = midnight, 0.5 = midday (-1: no cycle running). With GlintFollowsSun=1
    // the glint crosses the view by this.
    float dayFraction = -1;
    IDirect3DTexture9* shoreTexture = nullptr;
    float shoreGrid[2] = {};
    // Caustics can be drawn (their shaders and textures loaded, and switched on): the shore texture is wanted.
    bool ShoreWanted() const { return m_shorePs && m_caustic[0] && g_config.caustics > 0; }
    // Where the glint's light is for a view (vp: world -> clip, register form), from sunDirection and dayFraction
    // ([water] GlintFollowsSun): xyz towards it (unit), w how bright the glint is (GlintFade). Colonization's painted
    // sun without a sun to follow. The sea's glint and wave shading use it, and the rivers' glint.
    void GlintLight(const float vp[16], float toLight[4]) const;
    // The sea's glint for a view, for the rivers' too: the sky texture (its alpha the painted sun; null if the water
    // can't be loaded), and the turn (water.hlsl's mtxGlint, 3 registers) that puts the painted sun where the glint is
    // now and how bright the glint is (GlintFade).
    IDirect3DBaseTexture9* GlintSky(const float vp[16], float rot[12], float& fade);
    // The normal maps: water_normal.dds (0, at Colonization's tiling) and any water_normal_1.dds, _2... found where it
    // is, each drawn at 1440 world units (8 plots) per repeat. NextNormalMap cycles through them ([water]
    // NormalMapToggleKey); [water] NormalMap picks the one at the start.
    size_t NormalMapCount() const { return 1 + m_numbered.size(); }
    void NextNormalMap();

    // Loads the shaders and textures on first use. False if the water textures can't be found (logged once); the
    // caller then leaves BtS's water alone.
    bool Ready();
    void NewFrame() { m_snapshotTaken = false; }
    // Copy what the map is being drawn into (the back buffer, or the screen-sized texture behind the Civilopedia) into
    // the refraction texture, once per frame. Call before the first ship or water draw.
    void SnapshotRefraction();
    bool SnapshotTaken() const { return m_snapshotTaken; }
    // After a ship's or submarine's draw (before the water): its part below the surface into the refraction, so it
    // shows through the water, fading with depth ([water] Underwater*). vs = its shader (null: fixed function).
    bool DrawSubmerged(const ShaderInfo* vs, float waterHeight, const std::function<void()>& redraw);
    // debug: the refraction (screen copy plus what's under the surface) into a trace folder
    bool SaveRefractionBmp(const std::wstring& path);
    // Draw one BtS water draw with our water shader: set its shaders, constants and textures, call draw(), restore
    // BtS's state. reflection may be null (then the water reflects only the sky).
    HRESULT Draw(const ShaderInfo& btsVs, const ShaderInfo* btsPs, IDirect3DTexture9* reflection, float waterHeight,
                 const std::function<HRESULT()>& draw);
    // Right after BtS's fixed-function draw of water on a model (an oasis' pool: Shadows::WaterSurface), with its
    // state still set: draw it again on top with the water's ripples, sky reflection and sun glint
    // (water_surface.hlsl). False if the mesh isn't one it can redraw (no xyz positions or 2D texture coordinates for
    // stage 0, or its texture coordinates generated or projected); BtS's draw then stands as it is.
    bool DrawSurface(const std::function<HRESULT()>& draw);

    void ReleaseDefault();  // before Reset: the refraction render target (made again on use)
    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    bool m_loggedSun = false;
    bool Load();
    void LoadShore(const std::function<bool(const std::vector<uint8_t>&, IDirect3DTexture9**)>& texture);
    bool EnsureRefraction();

    IDirect3DDevice9* m_dev;
    bool m_tried = false, m_ok = false;
    IDirect3DVertexShader9* m_vs = nullptr;
    IDirect3DPixelShader9* m_ps = nullptr;
    ShaderInfo m_vsInfo, m_psInfo;
    IDirect3DVertexShader9* m_surfaceVs = nullptr;  // water_surface.hlsl
    IDirect3DPixelShader9* m_surfacePs = nullptr;
    bool m_loggedSurface = false;
    IDirect3DVertexShader9* m_shoreVs = nullptr;  // water.hlsl with SHORE (Shader Model 3)
    IDirect3DPixelShader9* m_shorePs = nullptr;
    ShaderInfo m_shoreVsInfo, m_shorePsInfo;
    IDirect3DTexture9* m_caustic[2] = {};  // Civilization V's caustic networks
    bool m_loggedShore = false;
    // GlintFollowsSun=1 with the light cycle: how far round the glint has gone, right to left (0 and 1 behind the
    // camera, 0.5 the middle of the view), the sun's by day and the moon's by night (from dayFraction).
    float GlintAcross() const;
    // Where that puts it: radians to the right of the camera's heading (steady across the view, fast round the back)...
    float GlintSide() const;
    // ...and how low on the screen, 0 (the middle of the view) to 1 (its edges, and round the back).
    float GlintLow() const;
    // And how bright it is: dimming near the edges of the view, gone out of it (1 otherwise).
    float GlintFade() const;
    // The turn (mtxGlint, 3 registers, left alone when there is none) that puts the painted sun at toSun (unit).
    static void GlintTurn(const float toSun[3], float rot[12]);
    int m_glintNight = -1;  // the glint last logged: 0 the sun's, 1 the moon's
    IDirect3DTexture9* m_normal = nullptr;
    std::vector<IDirect3DTexture9*> m_numbered;  // water_normal_1.dds, _2...
    std::vector<std::wstring> m_numberedFrom;
    size_t m_normalMap = 0;                      // in use: 0 = m_normal, i = m_numbered[i - 1]
    IDirect3DTexture9* CurrentNormal() const { return m_normalMap ? m_numbered[m_normalMap - 1] : m_normal; }
    // Whether each map is DXT5nm (x in alpha, y in green; the shaders' f4WaterConstants.w).
    bool m_normalNm = false;
    std::vector<bool> m_numberedNm;
    bool CurrentNormalNm() const { return m_normalMap ? m_numberedNm[m_normalMap - 1] : m_normalNm; }
    // The two normal-map layers' texture matrices (mtxWaterTexture1/2) at this time, for the map in use.
    void NormalLayers(float time, float tex1[12], float tex2[12]) const;
    IDirect3DCubeTexture9* m_env = nullptr;
    IDirect3DTexture9* m_black = nullptr;  // grid (off) and refraction fallback
    IDirect3DTexture9* m_clear = nullptr;  // reflection fallback: alpha 0 = sky only
    IDirect3DTexture9* m_refr = nullptr;
    IDirect3DSurface9* m_refrSurf = nullptr;
    ULONG m_internalRefs = 0;
    bool m_snapshotTaken = false;
    DWORD m_startTick = 0;
};
