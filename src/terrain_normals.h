// Terrain normals for per-pixel terrain lighting.
//
// BtS's terrain has no normals (its lighting is baked into a lightmap at map load). Its geometry is a regular grid,
// drawn as one mesh per splat layer and chunk, each with its own static vertex buffer (position + 2 uv, 28 bytes)
// placed by a translation-only mtxWorld (VS c4-c6). The first time a terrain vertex buffer is drawn, its vertices
// are read once and their world heights recorded in a grid; normals come from the heights of neighbouring cells, and
// live in a texture (world-aligned, wrapping) that a terrain shader can sample per pixel.
//
// The heights also give hill shadows, worked out on the GPU when the sun, the darkness or the view has changed
// enough to see (shaders\hill_shadow.hlsl) from a height
// texture on the same grid, into a mask texture (a = shadow, rgb = the light factor 1 - darkness x shadow) that the
// terrain, rivers and ground decals are darkened by. Needs Shader Model 3.
#pragma once
#include "common.h"
#include "terrain_cells.h"
#include "trace.h"

#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TerrainNormals
{
public:
    explicit TerrainNormals(IDirect3DDevice9* device) : m_dev(device) {}
    ~TerrainNormals() { ReleaseAll(); }

    // A BtS terrain draw is about to happen: read its vertex buffer if it's new, or new in this place (BtS draws
    // some buffers at more than one cell, moved by mtxWorld).
    void Capture(const ShaderInfo& vs);
    // A vertex buffer was just created at this address: forget an earlier one there.
    void ForgetBuffer(const void* vb) { m_done.erase(vb); }
    // The normal texture (A8R8G8B8: rgb = normal x 0.5 + 0.5, a = 255 where known), its world spacing per texel and
    // size in texels; null until something was captured.
    IDirect3DTexture9* Texture();
    // Once per frame, outside a scene: draw the hill shadow mask for this sun (travel direction) and darkness over
    // the ground in view (centre x, y and half-width in world units), if they changed enough to see (true if it did).
    // HillTexture: the mask, or
    // null (no terrain read yet, or no Shader Model 3); HillGrid: world xy -> its uv (scale, x offset, y offset).
    bool UpdateHills(const float* sunDirection, float darkness, const float* view);
    // Why the last mask was made (kHill* bits). The mask's area is the view and a margin, kept while the view stays
    // inside it (and isn't much smaller), so small pans and the view's own wobble don't make a new one.
    enum
    {
        kHillFirst = 1,     // none yet (or after a Reset)
        kHillTerrain = 2,   // new terrain read
        kHillSun = 4,       // the sun moved more than 0.1 degrees
        kHillDarkness = 8,  // the darkness changed by half a colour step
        kHillView = 16,     // the view left the mask's area, or zoomed in on it
    };
    int HillWhy() const { return m_hillWhy; }
    // CPU seconds since the last call reading new terrain and updating the textures from it, and the bytes written into
    // them (a managed texture uploads just those); starts a new count.
    void TakeStats(double& readSeconds, double& uploadSeconds, double& uploadBytes)
    {
        readSeconds = m_readSeconds, uploadSeconds = m_uploadSeconds, uploadBytes = m_uploadBytes;
        m_readSeconds = m_uploadSeconds = m_uploadBytes = 0;
    }
    IDirect3DTexture9* HillTexture() const { return m_hillReady ? m_hillTex : nullptr; }
    const float* HillGrid() const { return m_hillGrid; }
    // The shore, for the water's caustics: a texture on the height grid (A8R8G8B8, wrapping), r = distance to
    // the nearest land (terrain at or above waterHeight) in cells / 32, g = depth below waterHeight / 32, a = 255 where
    // the terrain has been read. Null until something was. Worked out again around new terrain as it's read (all of it
    // when the water height changes).
    IDirect3DTexture9* ShoreTexture(float waterHeight);
    // The heights (R32F on the same grid, wrapping; unknown = -10000), new cells uploaded as terrain is read; null
    // until something was, or if the texture can't be made. The hill shadows and the waves' draping read it.
    IDirect3DTexture9* HeightTexture() { return m_spacing > 0 && EnsureHeightTexture() ? m_heightTex : nullptr; }
    float Spacing() const { return m_spacing; }
    UINT Cells() const { return m_cells; }
    // The grid's size for this spacing: the smallest power of two (kMinCells to kMaxCells) whose cells cover the map
    // (mapSpan world units across, from mtxLightmap; 0 if unknown) and at least a Huge map (128 plots), so a later,
    // bigger map loaded at the same spacing fits too. Maps wider than kMaxCells cells wrap onto themselves.
    static UINT CellsFor(float spacing, float mapSpan);
    // The grid's spacing for terrain whose vertices are vertexSpacing apart: every vertex while kMaxCells of them
    // cover the map (and a Huge map), else every 2nd, 4th... (the 65-sample terrain patch with High Detail Terrain is
    // 2.8 apart; lighting and hill shadows need no finer than High Detail Terrain's 11.25), so nothing wraps.
    static float GridSpacingFor(float vertexSpacing, float mapSpan);
    // Detail normals: the world xy direction in which the detail texture's u grows (xy) and its v (zw), unit length,
    // from the terrain's TEXCOORD1; null until a terrain vertex buffer was read.
    const float* DetailAxes() const { return m_haveDetailAxes ? m_detailAxes : nullptr; }

    bool SaveBmp(const std::wstring& path);  // debug: the normal map, for traces
    bool SaveHillBmp(const std::wstring& path);  // debug: the hill shadow mask
    // Debug: the height grid as raw little-endian floats, row by row (Cells() x Cells(); NaN = unknown).
    bool SaveHeights(const std::wstring& path) const
    {
        return !m_heights.empty() && WriteBytes(path, m_heights.data(), m_heights.size() * sizeof(float));
    }
    void ReleaseDefault();  // the mask target and state block, before a Reset (made again on use)
    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    // Grid and texture size: m_cells x spacing world units, wrapping. 1024 at BtS's default 22.5 spacing (128 plots);
    // 2048 at 11.25 ("High Detail Terrain"). 2048 costs 16 MB per copy of each grid (heights, normals and their
    // textures, the shore); 4096 would be 64 MB each, too much for a 32-bit game.
    static const UINT kMinCells = 1024, kMaxCells = 2048;
    static constexpr float kHugeMapSpan = 128 * 180.0f;  // a Huge map: 128 plots of 180 units
    static const UINT kHillCells = 2048;  // the hill shadow mask's size: over the ground in view
    size_t Index(int cx, int cy) const { return (cy & (m_cells - 1)) * m_cells + (cx & (m_cells - 1)); }
    float& Height(int cx, int cy) { return m_heights[Index(cx, cy)]; }
    // Writes fill(cx, cy) (a T per texel) into tex for every cell of r, locking only those texels (a managed texture
    // then uploads only them); counted in the upload stats.
    template <class T, class Fill>
    bool Upload(IDirect3DTexture9* tex, const TerrainCells::Rect& r, Fill fill);
    float MapSpan(const ShaderInfo& vs) const;  // the map's width from mtxLightmap, or 0
    void UpdateNormals(int x0, int y0, int x1, int y1);
    void FitDetailAxes(const BYTE* vertices, UINT count, UINT stride, const std::vector<float>& world);
    bool EnsureHillResources();
    bool EnsureHeightTexture();  // made and uploaded if the heights changed since

    IDirect3DDevice9* m_dev;
    // vertex buffers already read (or not readable), with the places (mtxWorld's translation) they were read at
    std::unordered_map<const void*, std::vector<std::array<float, 3>>> m_done;
    std::vector<float> m_heights;            // world z per cell; NaN = unknown
    float m_spacing = 0;                     // world units between grid cells (GridSpacingFor, from the first buffer)
    float m_vertexSpacing = 0;               // ...and between the terrain's own vertices
    UINT m_cells = kMinCells;                // the grid's size (CellsFor, from the first buffer)
    IDirect3DTexture9* m_tex = nullptr;
    ULONG m_internalRefs = 0;
    TerrainCells::Rect m_normalsNew;  // m_normals changed here since the last upload
    unsigned m_buffers = 0, m_vertices = 0, m_unreadable = 0;
    bool m_loggedUnreadable = false;
    std::vector<DWORD> m_normals;  // texture contents, kept to update dirty rectangles
    float m_minHeight = 1e30f, m_maxHeight = -1e30f;  // lowest and highest terrain read
    float m_detailAxes[4] = {};
    bool m_haveDetailAxes = false;
    bool m_loggedNoDetailUv = false;
    // Hill shadows: heights (R32F, unknown = -10000), the mask (a render target), the pass's shaders and state.
    IDirect3DTexture9* m_heightTex = nullptr;
    bool m_heightsDirty = true;     // new heights since the hill shadows last used them
    TerrainCells::Rect m_heightsNew;  // new heights here since the height texture was last uploaded
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
    int m_hillWhy = 0;
    // The shore texture, the cells read since it was last worked out, and all the cells read so far (bounding boxes).
    IDirect3DTexture9* m_shoreTex = nullptr;
    TerrainCells::Rect m_shoreNew;
    bool m_shoreAll = true;  // work out all of it (a new texture)
    float m_shoreHeight = 0;
    TerrainCells::Rect m_read;
    double m_readSeconds = 0, m_uploadSeconds = 0, m_uploadBytes = 0;  // TakeStats
};
