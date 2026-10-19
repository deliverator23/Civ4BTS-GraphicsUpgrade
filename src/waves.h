// BtS's coast waves drawn with our copy of their shader (shaders\wave.hlsl), which adds an opacity over time that a
// wave model can carry in its (otherwise unseen) base texture layer's animated offset, and (Shader Model 3, with
// vertex textures) drapes the waves over the terrain so the foam runs up the beach. Waves whose models don't carry an
// opacity, over open water, look exactly as the game draws them.
#pragma once
#include "common.h"
#include "trace.h"

#include <functional>

class Waves
{
public:
    explicit Waves(IDirect3DDevice9* device) : m_dev(device) {}
    ~Waves() { ReleaseAll(); }

    // For draping (set before a wave draw): the terrain's heights (TerrainNormals::HeightTexture; null: none read yet,
    // the waves stay flat) and the grid's spacing (world units) and size (cells).
    IDirect3DTexture9* heightTexture = nullptr;
    float heightSpacing = 0;
    UINT heightCells = 0;
    // The change of light the foam takes (Lighting::DecalLight: rgb; null = none), set before a wave draw; the foam
    // takes [water] FoamLight of it (0 = white as BtS draws it, 1 = all of it).
    const float* light = nullptr;

    // A draw with BtS's wave shader (Civ4Wave.fx): its constants and samplers where ours expect them.
    static bool IsWave(const ShaderInfo& vs, const ShaderInfo* ps);
    // Draw it with our shaders in place of the game's (then the game's back). False if ours couldn't be made: the
    // caller then draws it as usual.
    bool Draw(const std::function<HRESULT()>& draw, HRESULT& hr);

    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    bool EnsureShaders();
    bool EnsureDrape();  // the draped shaders, if the card can read an R32F texture in a vertex shader

    IDirect3DDevice9* m_dev;
    IDirect3DVertexShader9* m_vs = nullptr;
    IDirect3DPixelShader9* m_ps = nullptr;
    IDirect3DVertexShader9* m_drapeVs = nullptr;
    IDirect3DPixelShader9* m_drapePs = nullptr;
    bool m_failed = false, m_logged = false;
    bool m_drapeTried = false, m_loggedDrape = false;
    ULONG m_internalRefs = 0;
};
