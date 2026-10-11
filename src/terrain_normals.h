// Terrain normals for per-pixel terrain lighting.
//
// BtS's terrain has no normals (its lighting is baked into a lightmap at map load). Its geometry is a regular grid,
// drawn as one mesh per splat layer and chunk, each with its own static vertex buffer (position + 2 uv, 28 bytes)
// placed by a translation-only mtxWorld (VS c4-c6). The first time a terrain vertex buffer is drawn, its vertices
// are read once and their world heights recorded in a grid; normals come from the heights of neighbouring cells, and
// live in a texture (world-aligned, wrapping) that a terrain shader can sample per pixel.
//
// The heights also give hill shadows, worked out on the GPU every frame (shaders\hill_shadow.hlsl) from a height
// texture on the same grid, into a mask texture (a = shadow, rgb = the light factor 1 - darkness x shadow) that the
// terrain, rivers and ground decals are darkened by. Needs Shader Model 3.
#pragma once
#include "common.h"
#include "trace.h"

#include <unordered_set>
#include <vector>

class TerrainNormals
{
public:
    explicit TerrainNormals(IDirect3DDevice9* device) : m_dev(device) {}
    ~TerrainNormals() { ReleaseAll(); }

    // A BtS terrain draw is about to happen: read its vertex buffer if it's new.
    void Capture(const ShaderInfo& vs);
    // A vertex buffer was just created at this address: forget an earlier one there.
    void ForgetBuffer(const void* vb) { m_done.erase(vb); }
    // The normal texture (A8R8G8B8: rgb = normal x 0.5 + 0.5, a = 255 where known), its world spacing per texel and
    // size in texels; null until something was captured.
    IDirect3DTexture9* Texture();
    // Once per frame, outside a scene: draw the hill shadow mask for this sun (travel direction) and darkness over
    // the ground in view (centre x, y and half-width in world units), if anything changed. HillTexture: the mask, or
    // null (no terrain read yet, or no Shader Model 3); HillGrid: world xy -> its uv (scale, x offset, y offset).
    void UpdateHills(const float* sunDirection, float darkness, const float* view);
    IDirect3DTexture9* HillTexture() const { return m_hillReady ? m_hillTex : nullptr; }
    const float* HillGrid() const { return m_hillGrid; }
    float Spacing() const { return m_spacing; }
    UINT Cells() const { return kCells; }

    bool SaveBmp(const std::wstring& path);  // debug: the normal map, for traces
    bool SaveHillBmp(const std::wstring& path);  // debug: the hill shadow mask
    // Debug: the height grid as raw little-endian floats, row by row (kCells x kCells; NaN = unknown).
    bool SaveHeights(const std::wstring& path) const
    {
        return !m_heights.empty() && WriteBytes(path, m_heights.data(), m_heights.size() * sizeof(float));
    }
    void ReleaseDefault();  // the mask target and state block, before a Reset (made again on use)
    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    static const UINT kCells = 1024;  // grid and texture size: 1024 x spacing world units, wrapping
    static const UINT kHillCells = 2048;  // the hill shadow mask's size: over the ground in view
    float& Height(int cx, int cy) { return m_heights[(cy & (kCells - 1)) * kCells + (cx & (kCells - 1))]; }
    void UpdateNormals(int x0, int y0, int x1, int y1);
    bool EnsureHillResources();

    IDirect3DDevice9* m_dev;
    std::unordered_set<const void*> m_done;  // vertex buffers already read (or not readable)
    std::vector<float> m_heights;            // world z per cell; NaN = unknown
    float m_spacing = 0;                     // world units between grid vertices (from the first buffer)
    IDirect3DTexture9* m_tex = nullptr;
    ULONG m_internalRefs = 0;
    bool m_dirty = false;  // m_normals changed since the last upload
    unsigned m_buffers = 0, m_vertices = 0, m_unreadable = 0;
    bool m_loggedUnreadable = false;
    std::vector<DWORD> m_normals;  // texture contents, kept to update dirty rectangles
    float m_minHeight = 1e30f, m_maxHeight = -1e30f;  // lowest and highest terrain read
    // Hill shadows: heights (R32F, unknown = -10000), the mask (a render target), the pass's shaders and state.
    IDirect3DTexture9* m_heightTex = nullptr;
    bool m_heightsDirty = true;
    bool m_heightFilter = true;  // R32F can be filtered (bilinear), else point
    IDirect3DTexture9* m_hillTex = nullptr;
    IDirect3DSurface9* m_hillSurf = nullptr;
    IDirect3DVertexShader9* m_hillVs = nullptr;
    IDirect3DPixelShader9* m_hillPs = nullptr;
    IDirect3DStateBlock9* m_hillState = nullptr;
    bool m_hillFailed = false;  // creating them failed: no hill shadows
    bool m_hillReady = false;   // the mask holds a result for:
    float m_hillSun[3] = {}, m_hillDarkness = -1, m_hillRegion[3] = {};
    float m_hillGrid[3] = {};
};
