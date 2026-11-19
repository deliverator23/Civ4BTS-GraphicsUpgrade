// BtS's area borders, the settler's city-site guide above all, laid over the terrain ([textures] AreaBorderTextures):
// a fixed-function, indexed triangle-list draw with one of the listed textures on stage 0 is cut up and draped over
// the terrain's heights (area_border_drape.h), then drawn from our copy with the depth test on (no depth writes), the
// game's other settings as they are. Anything it can't take (no heights under it yet, a vertex format other than plain
// positions, a buffer it can't read) is drawn as the game draws it.
#pragma once
#include "assets.h"
#include "common.h"
#include "terrain_normals.h"

#include <cstdint>
#include <vector>

class AreaBorders
{
public:
    explicit AreaBorders(IDirect3DDevice9* device) : m_dev(device) {}

    bool enabled = false;  // something is listed in [textures] AreaBorderTextures (and effects allowed in this game)
    TextureList textures;  // [textures] AreaBorderTextures

    // The draw about to happen has a listed texture on stage 0.
    bool Listed();
    void ForgetTexture(const void* texture) { textures.Forget(texture); }

    // The game's DrawIndexedPrimitive (its arguments; the buffers, FVF and transforms as set) draped over terrain's
    // heights and drawn here (true, with the result in hr), or false: the caller draws it as the game does.
    bool Draw(const TerrainNormals& terrain, D3DPRIMITIVETYPE type, INT base, UINT minVertex, UINT numVertices,
              UINT start, UINT count, HRESULT& hr);
    const Drape::Stats& LastStats() const { return m_stats; }

private:
    void LogOnce(int reason, const char* text);

    IDirect3DDevice9* m_dev;
    Drape::Stats m_stats;
    std::vector<uint8_t> m_source, m_verts;
    std::vector<uint32_t> m_tris;
    std::vector<uint16_t> m_indices;
    unsigned m_logged = 0;  // reasons logged (bits)
};
