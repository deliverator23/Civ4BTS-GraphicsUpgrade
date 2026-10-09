// Terrain normals for per-pixel terrain lighting.
//
// BtS's terrain has no normals (its lighting is baked into a lightmap at map load). Its geometry is a regular grid,
// drawn as one mesh per splat layer and chunk, each with its own static vertex buffer (position + 2 uv, 28 bytes)
// placed by a translation-only mtxWorld (VS c4-c6). The first time a terrain vertex buffer is drawn, its vertices
// are read once and their world heights recorded in a grid; normals come from the heights of neighbouring cells, and
// live in a texture (world-aligned, wrapping) that a terrain shader can sample per pixel.
//
// The heights also give hill shadows: for every cell, step towards the sun through the height grid; where the terrain
// rises above that line the cell is shaded (softly: by how far it rises, up to HillShadowSoftness). The mask lives
// in a second texture on the same grid (a = shadow, rgb = the light factor 1 - darkness x shadow), recomputed
// around newly read terrain, and everywhere (over several frames) when the sun moves.
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
    // The hill shadow mask for this sun (travel direction) and darkness. A new direction is worked through over
    // several frames (a band of rows per frame); call NewFrame once per frame. Null until terrain was read.
    IDirect3DTexture9* HillTexture(const float* sunDirection, float darkness);
    void NewFrame() { m_hillBudget = true; }
    float Spacing() const { return m_spacing; }
    UINT Cells() const { return kCells; }

    bool SaveBmp(const std::wstring& path);  // debug: the normal map, for traces
    bool SaveHillBmp(const std::wstring& path);  // debug: the hill shadow mask
    void ReleaseAll();
    ULONG InternalRefs() const { return m_internalRefs; }

private:
    static const UINT kCells = 1024;  // grid and texture size: 1024 x spacing world units, wrapping
    float& Height(int cx, int cy) { return m_heights[(cy & (kCells - 1)) * kCells + (cx & (kCells - 1))]; }
    void UpdateNormals(int x0, int y0, int x1, int y1);
    void UpdateHills(int x0, int y0, int x1, int y1);  // recompute the mask in this cell range (wrapping)
    float HeightAt(float x, float y);                  // bilinear in cells; NaN if a corner is unknown

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
    std::vector<DWORD> m_hill;     // hill shadow mask contents
    IDirect3DTexture9* m_hillTex = nullptr;
    bool m_hillUpload = false;     // m_hill changed since the last upload
    bool m_hillPending = false;    // a region waits to be recomputed:
    int m_hillX0 = 0, m_hillY0 = 0, m_hillX1 = -1, m_hillY1 = -1;
    float m_hillSun[3] = {}, m_hillDarkness = -1, m_hillSoftness = -1;  // what the mask was computed for
    int m_hillRefreshRow = -1;  // next row of a gradual refresh for a moved sun (-1: none)
    bool m_hillBudget = true;   // this frame's band of rows not yet done
    void RelightHills();        // rgb from alpha again for a new darkness
    void MarkHillRows(int y0, int y1);  // rows to upload (outside the grid: all of them)
    int m_uploadY0 = 0, m_uploadY1 = 0;
    float m_minHeight = 1e30f, m_maxHeight = -1e30f;
};
