// BtS's culture borders drawn with our copy of their shader (shaders\border.hlsl) when [debug] TerrainVertsPerPlot
// makes the terrain finer than the border's own grid (17 x 17 a plot): each vertex lifted onto the terrain's heights
// and moved towards the camera along its line of sight, so the terrain's finer relief doesn't rise through the border.
// Shader Model 3 with vertex textures; otherwise, and with the stock terrain, the borders are drawn as the game draws
// them.
#pragma once
#include "common.h"
#include "trace.h"

#include <functional>

class Borders
{
public:
    explicit Borders(IDirect3DDevice9* device) : m_dev(device) {}
    ~Borders() { ReleaseAll(); }

    bool enabled = false;  // [debug] TerrainVertsPerPlot 33 or 65 (and effects allowed in this game)
    // Set before a border draw: the terrain's heights (TerrainNormals::HeightTexture; null: none read yet, the border
    // isn't lifted) and the grid's spacing (world units) and size (cells).
    IDirect3DTexture9* heightTexture = nullptr;
    float heightSpacing = 0;
    UINT heightCells = 0;

    // A draw with BtS's culture border shader (CultureBOrderShader.fx, either pass): its constants and samplers where
    // ours expect them.
    static bool IsBorder(const ShaderInfo& vs, const ShaderInfo* ps);
    // Draw it with our shaders in place of the game's (then the game's back). False if ours couldn't be made or the
    // camera couldn't be read: the caller then draws it as usual.
    bool Draw(const std::function<HRESULT()>& draw, HRESULT& hr);

    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    bool EnsureShaders();  // if the card can read an R32F texture in a vertex shader

    IDirect3DDevice9* m_dev;
    IDirect3DVertexShader9* m_vs = nullptr;
    IDirect3DPixelShader9* m_ps = nullptr;
    bool m_tried = false, m_logged = false, m_loggedHeights = false;
    ULONG m_internalRefs = 0;
};
