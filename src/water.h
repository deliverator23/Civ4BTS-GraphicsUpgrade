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
// Textures: Colonization's normal map and environment cube (GraphicsUpgrade\water_001.dds and water_env.dds, or read
// from a Colonization install's Art0.FPK; loaded with the game's own d3dx9_33.dll), BtS's coast fade and fog of war,
// the reflection, and a refraction texture copied from the back buffer just before the ships are drawn (BtS has drawn
// the sea floor by then).
#pragma once
#include "common.h"
#include "trace.h"

#include <functional>

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

    // Loads the shaders and textures on first use. False if the water textures can't be found (logged once); the
    // caller then leaves BtS's water alone.
    bool Ready();
    void NewFrame() { m_snapshotTaken = false; }
    // Copy the back buffer into the refraction texture, once per frame. Call before the first ship or water draw.
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

    void ReleaseDefault();  // before Reset: the refraction render target (made again on use)
    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    bool m_loggedSun = false;
    bool Load();
    bool EnsureRefraction();

    IDirect3DDevice9* m_dev;
    bool m_tried = false, m_ok = false;
    IDirect3DVertexShader9* m_vs = nullptr;
    IDirect3DPixelShader9* m_ps = nullptr;
    ShaderInfo m_vsInfo, m_psInfo;
    IDirect3DTexture9* m_normal = nullptr;
    IDirect3DCubeTexture9* m_env = nullptr;
    IDirect3DTexture9* m_black = nullptr;  // grid (off) and refraction fallback
    IDirect3DTexture9* m_clear = nullptr;  // reflection fallback: alpha 0 = sky only
    IDirect3DTexture9* m_refr = nullptr;
    IDirect3DSurface9* m_refrSurf = nullptr;
    ULONG m_internalRefs = 0;
    bool m_snapshotTaken = false;
    DWORD m_startTick = 0;
};
